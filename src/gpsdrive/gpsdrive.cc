#include "gpsdrive/gpsdrive.h"

#include <sys/time.h>

#include <Eigen/Dense>

#include "gpsdrive/config.h"
#include "hw/car/car.h"
#include "hw/gps/ubx.h"
#include "hw/imu/imu.h"
#include "hw/imu/mag.h"
#include "hw/input/js.h"
#include "inih/cpp/INIReader.h"
#include "inih/ini.h"
#include "ui/display.h"

const Eigen::Vector3f MAGCALN(3.202, -0.3750, 0.8825);
const Eigen::Vector3f MAGCALE(1.091, 2.869, -0.6832);

template <typename T>
T clamp(T x, T min, T max) {
  if (x < min) x = min;
  if (x > max) x = max;
  return x;
}

GPSDrive::GPSDrive(FlushThread *ft, IMU *imu, Magnetometer *mag,
                   JoystickInput *js, UIDisplay *disp)
    : flush_thread_(ft),
      imu_(imu),
      mag_(mag),
      js_(js),
      display_(disp),
      gyro_last_(0, 0, 0),
      gyro_bias_(0, 0, 0),
      gps_v_(0, 0, 0) {
  done_ = false;
  record_fp_ = NULL;
  js_throttle_ = 0;
  js_steering_ = 0;
  config_item_ = 0;
  ierr_k_ = 0;
  ierr_v_ = 0;
  last_u_esc_ = 0;
  brake_count_ = 0;
  last_v_ = 0;
  last_w_ = 0;
  lat_ = lon_ = 0;
  numSV_ = 0;

  autodrive_ = false;
  x_down_ = y_down_ = false;
  pthread_mutex_init(&record_mut_, NULL);
}

void* GPSDrive::gpsThread(void* arg) {
  GPSDrive *drive = (GPSDrive*)arg;
  fprintf(stderr, "GPS receive thread started\n");
  ubx_read_loop(drive->ubx_fd_, drive);
  return NULL;
}

bool GPSDrive::Init(const INIReader &ini) {
  if (config_.Load()) {
    fprintf(stderr, "Loaded driver configuration\n");
  }

  ubx_fd_ = ubx_open();
  if (ubx_fd_ == -1) {
    return false;
  }

  if (pthread_create(&gps_thread_, NULL, GPSDrive::gpsThread, (void*) this)) {
    perror("pthread_create");
    return false;
  }

  ref_lat_ = ini.GetInteger("nav", "reflat", 0);
  ref_lon_ = ini.GetInteger("nav", "reflon", 0);
  if (ref_lat_ == 0 || ref_lon_ == 0) {
    fprintf(stderr, "Please provide [nav] reflat and reflon in cycloid.ini\n");
    fprintf(stderr, "note: they are integers, w/ 7 decimal places\n");
    return false;
  }
  // compute meters / 1e-7 degree on WGS84 ellipsoid
  // this is an approximation that assumes 0 altitude
  double invf = 298.257223563;                  // WGS84 inverse flattening
  double a = 6378137.0;                         // meters
  double b = a * (1 - 1/invf);
  double lat = ref_lat_ * M_PI * 1e-7 / 180.0;  // lat/lon in radians
  double clat = cos(lat);

  mscale_lat_ = b * M_PI / 180.0e7;
  mscale_lon_ = a * clat * M_PI / 180.0e7;

  // draw UI screen
  UpdateDisplay();
  display_->UpdateStatus("GPSDrive started.");

  return true;
}

GPSDrive::~GPSDrive() {}

bool GPSDrive::OnControlFrame(CarHW *car, float dt) {
  if (js_) {
    js_->ReadInput(this);
  }

  Eigen::Vector3f accel, gyro, mag;
  if (!imu_->ReadIMU(&accel, &gyro)) {
    fprintf(stderr, "imu read failure\n");
    accel = accel.Zero();
    gyro = gyro.Zero();
  } else {
    gyro_last_ = 0.95 * gyro_last_ + 0.05 * gyro;
  }
  gyro -= gyro_bias_;

  if (!mag_->ReadMag(&mag)) {
    fprintf(stderr, "magnetometer read failure\n");
    mag = mag.Zero();
  }
  float MagN = mag.dot(MAGCALN);
  float MagE = mag.dot(MAGCALE);
  float renorm = sqrtf(MagN*MagN + MagE*MagE);
  MagN /= renorm;
  MagE /= renorm;

  bool radio_safe = false;  // runaway protection

  float controls[2] = {0, 0};
  float u_throttle = 0;
  float u_steering = 0;
  if (car->GetRadioInput(controls, 2)) {
    u_throttle = controls[0];
    u_steering = controls[1];
    if (u_throttle > 0.5) {
      radio_safe = true;
    }
  } else {
    u_throttle = js_throttle_ / 32768.0;
    u_steering = js_steering_ / 32760.0;
  }

  float u_s = clamp(u_steering + config_.servo_offset * 0.01f,
                    config_.servo_min * 0.01f, config_.servo_max * 0.01f);

  float ds = 0, v = 0;
  float w = gyro[2];
  car->GetWheelMotion(&ds, &v);

  if (brake_count_ > 0) {
    brake_count_--;
    // dumb assumption: we rapidly decay speed estimate when we hit the brake
    // so that we "pump" the brakes so we can see how fast we're going
    // v = last_v_ * 0.95;
    // this isn't going to work
    v = last_v_ * 0.95;
    // we need to use GPS velocity here
  }

  if (record_fp_ != NULL) {
    timeval tv;
    gettimeofday(&tv, NULL);
    pthread_mutex_lock(&record_mut_);
    fprintf(record_fp_,
            "%ld.%06ld control %f %f wheel %f %f imu %f %f %f %f %f %f mag %f "
            "%f %f\n",
            tv.tv_sec, tv.tv_usec, u_throttle, u_steering, ds, v, accel[0],
            accel[1], accel[2], gyro[0], gyro[1], gyro[2], mag[0], mag[1],
            mag[2]);
    pthread_mutex_unlock(&record_mut_);
  }

  if (display_) {
    display_->UpdateDashboard(v, w, lon_, lat_, numSV_, gps_v_.norm(),
                              (lon_ - ref_lon_) * mscale_lon_,
                              (lat_ - ref_lat_) * mscale_lat_, MagN, MagE);
  }

  if (!autodrive_ && u_throttle <= 0.05) {
    car->SetControls(2, u_throttle, u_s);
    if (u_throttle < -0.05) {
      brake_count_ = 5;
    }
    ierr_v_ = 0;
    ierr_k_ = 0;
    last_v_ = v;
    last_w_ = w;
    last_u_esc_ = u_throttle;
    return !done_;
  }

  if (autodrive_ && !radio_safe) {
    car->SetControls(2, 0, 0);
    return !done_;
  }

  float target_v = config_.speed_limit * 0.01 * clamp(u_throttle, 0.f, 1.f);
  float vgain = 0.01 * config_.motor_gain;
  float kI = 0.01 * config_.motor_kI;
  float verr = target_v - v;
  float u = vgain * verr + kI * (ierr_v_ + verr * dt);
  if (u > -1 && u < 1) {
    ierr_v_ += verr * dt;
  }
  if (target_v < v * 0.9) {
    u = clamp(u, -1.f, 1.f);
  } else {
    u = clamp(u, 0.f, 1.f);
  }
  car->SetControls(1, u, u_s);

  last_v_ = v;
  last_w_ = w;
  last_u_esc_ = u;
  if (u < -0.05) {
    brake_count_ = 5;
  }
  return !done_;
}

void GPSDrive::OnNav(const nav_pvt &msg) {
  if (record_fp_ != NULL) {
    timeval tv;
    gettimeofday(&tv, NULL);
    pthread_mutex_lock(&record_mut_);
    fprintf(record_fp_, "%ld.%06ld gps ", tv.tv_sec, tv.tv_usec);
    fprintf(record_fp_, "%04d-%02d-%02dT%02d:%02d:%02d.%09d ", msg.year,
            msg.month, msg.day, msg.hour, msg.min, msg.sec, msg.nano);
    fprintf(record_fp_,
        "fix:%d numSV:%d %d.%07d +-%dmm %d.%07d +-%dmm height %dmm "
        "vel %d %d %d +-%d mm/s "
        "heading motion %d.%05d vehicle %d +- %d.%05d\n",
        msg.fixType, msg.numSV, msg.lon / 10000000,
        std::abs(msg.lon) % 10000000, msg.hAcc, msg.lat / 10000000,
        std::abs(msg.lat) % 10000000, msg.vAcc, msg.height, msg.velN, msg.velE,
        msg.velD, msg.sAcc, msg.headMot / 100000,
        std::abs(msg.headMot) % 100000, msg.headVeh, msg.headAcc / 100000,
        msg.headAcc % 100000);
    pthread_mutex_unlock(&record_mut_);
  }

  lat_ = msg.lat;
  lon_ = msg.lon;
  numSV_ = msg.numSV;
  gps_v_ = Eigen::Vector3f(msg.velN, msg.velE, msg.velD) * 0.001f;

  // TODO:
  //  - lookup closest point on target trajectory
  //  - compute track_ye, track_psie, track_k
}

void GPSDrive::OnDPadPress(char direction) {
  int16_t *value = ((int16_t *)&config_) + config_item_;
  switch (direction) {
    case 'U':
      --config_item_;
      if (config_item_ < 0) config_item_ = DriverConfig::N_CONFIGITEMS - 1;
      fprintf(stderr, "\n");
      break;
    case 'D':
      ++config_item_;
      if (config_item_ >= DriverConfig::N_CONFIGITEMS) config_item_ = 0;
      fprintf(stderr, "\n");
      break;
    case 'L':
      if (y_down_) {
        *value -= 100;
      } else if (x_down_) {
        *value -= 10;
      } else {
        --*value;
      }
      break;
    case 'R':
      if (y_down_) {
        *value += 100;
      } else if (x_down_) {
        *value += 10;
      } else {
        ++*value;
      }
      break;
  }
  UpdateDisplay();
}

void GPSDrive::OnButtonPress(char button) {
  switch (button) {
    case '+':  // start button
      StartRecording();
      break;
    case '-':  // stop button
      StopRecording();
      break;
    case 'B':
      if (config_.Load()) {
        fprintf(stderr, "config loaded\n");
        int16_t *values = ((int16_t *)&config_);
        if (display_) {
          display_->UpdateConfig(DriverConfig::confignames,
                                 DriverConfig::N_CONFIGITEMS, config_item_,
                                 values);
          display_->UpdateStatus("config loaded", 0xffff);
        }
      }
      fprintf(stderr, "reset kalman filter\n");
      break;
    case 'A':
      if (config_.Save()) {
        fprintf(stderr, "config saved\n");
        if (display_) display_->UpdateStatus("config saved", 0xffff);
      }
      break;
    case 'R':
      if (display_) {
        display_->NextMode();
      }
      break;
    case 'X':
      x_down_ = true;
      break;
    case 'Y':
      y_down_ = true;
      break;
    case 'H':  // home button: init to start line
      gyro_bias_ = gyro_last_;
      break;
  }
}

void GPSDrive::OnButtonRelease(char button) {
  switch(button) {
    case 'X':
      x_down_ = false;
      break;
    case 'Y':
      y_down_ = false;
      break;
  }
}

void GPSDrive::OnAxisMove(int axis, int16_t value) {
  switch (axis) {
    case 1:  // left stick y axis
      js_throttle_ = -value;
      break;
    case 2:  // right stick x axis
      js_steering_ = value;
      break;
  }
}

void GPSDrive::StartRecording() {
  timeval tv;
  gettimeofday(&tv, NULL);

  if (record_fp_ != NULL) {
    return;
  }

  char fnamebuf[256];
  time_t start_time = time(NULL);
  struct tm start_time_tm;
  localtime_r(&start_time, &start_time_tm);
  strftime(fnamebuf, sizeof(fnamebuf), "gpsdrive-%Y%m%d-%H%M%S.log",
           &start_time_tm);
  record_fp_ = fopen(fnamebuf, "w");
  if (!record_fp_) {
    perror(fnamebuf);
  }

  printf("%ld.%06ld start recording %s\n", tv.tv_sec, tv.tv_usec, fnamebuf);
  display_->UpdateStatus(fnamebuf);
}

void GPSDrive::StopRecording() {
  if (record_fp_ == NULL) {
    return;
  }

  timeval tv;
  gettimeofday(&tv, NULL);

  pthread_mutex_lock(&record_mut_);
  fclose(record_fp_);
  record_fp_ = NULL;
  pthread_mutex_unlock(&record_mut_);

  printf("%ld.%06ld stop recording\n", tv.tv_sec, tv.tv_usec);
  display_->UpdateStatus("stop recording");
}

void GPSDrive::UpdateDisplay() {
  // hack because all config values are int16_t's in 1/100th steps
  int16_t *values = ((int16_t *)&config_);
  int16_t value = values[config_item_];
  // FIXME: does this work for negative values?
  fprintf(stderr, "%s %d.%02d\r", DriverConfig::confignames[config_item_], value / 100,
          value % 100);

  if (display_)
    display_->UpdateConfig(DriverConfig::confignames,
                           DriverConfig::N_CONFIGITEMS, config_item_, values);
}

void GPSDrive::Quit() {
  done_ = true;
  StopRecording();
}