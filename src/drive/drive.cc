#include <fcntl.h>
#include <fenv.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "coneslam/imgproc.h"
#include "coneslam/localize.h"
#include "drive/config.h"
#include "drive/controller.h"
#include "drive/flushthread.h"
#include "hw/cam/cam.h"
// #include "hw/car/pca9685.h"
#include "hw/car/teensy.h"
#include "hw/imu/imu.h"
#include "hw/input/js.h"
#include "ui/display.h"

// #undef this to disable camera, just to record w/ raspivid while
// driving around w/ controller
#define CAMERA 1

const int NUM_PARTICLES = 300;

volatile bool done = false;

// ugh ugh ugh
int8_t throttle_ = 0, steering_ = 0;
int16_t js_throttle_ = 0, js_steering_ = 0;

// const int PWMCHAN_STEERING = 14;
// const int PWMCHAN_ESC = 15;

void handle_sigint(int signo) { done = true; }

I2C i2c;
// PCA9685 pca(i2c);
Teensy teensy(i2c);
IMU imu(i2c);
UIDisplay display_;
FlushThread flush_thread_;
Eigen::Vector3f accel_(0, 0, 0), gyro_(0, 0, 0);
uint8_t servo_pos_ = 110;
uint16_t wheel_pos_[4] = {0, 0, 0, 0};
uint16_t wheel_dt_[4] = {0, 0, 0, 0};

class Driver: public CameraReceiver {
 public:
  Driver(coneslam::Localizer *loc) {
    output_fd_ = -1;
    frame_ = 0;
    frameskip_ = 0;
    autodrive_ = false;
    gettimeofday(&last_t_, NULL);
    if (config_.Load()) {
      fprintf(stderr, "Loaded driver configuration\n");
    }
    localizer_ = loc;
    firstframe_ = true;
  }

  bool StartRecording(const char *fname, int frameskip) {
    frameskip_ = frameskip;
    if (!strcmp(fname, "-")) {
      output_fd_ = fileno(stdout);
    } else {
      output_fd_ = open(fname, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    }
    if (output_fd_ == -1) {
      perror(fname);
      return false;
    }
    return true;
  }

  bool IsRecording() {
    return output_fd_ != -1;
  }

  void StopRecording() {
    if (output_fd_ == -1) {
      return;
    }
    flush_thread_.AddEntry(output_fd_, NULL, -1);
    output_fd_ = -1;
  }

  ~Driver() {
    StopRecording();
  }

  void OnFrame(uint8_t *buf, size_t length) {
    struct timeval t;
    gettimeofday(&t, NULL);
    frame_++;

    if (IsRecording() && frame_ > frameskip_) {
      frame_ = 0;
      uint32_t flushlen = 55 + length;
      // copy our frame, push it onto a stack to be flushed
      // asynchronously to sdcard
      uint8_t *flushbuf = new uint8_t[flushlen];
      memcpy(flushbuf, &flushlen, 4);  // write header length
      memcpy(flushbuf+4, &t.tv_sec, 4);
      memcpy(flushbuf+8, &t.tv_usec, 4);
      memcpy(flushbuf+12, &throttle_, 1);
      memcpy(flushbuf+13, &steering_, 1);
      memcpy(flushbuf+14, &accel_[0], 4);
      memcpy(flushbuf+14+4, &accel_[1], 4);
      memcpy(flushbuf+14+8, &accel_[2], 4);
      memcpy(flushbuf+26, &gyro_[0], 4);
      memcpy(flushbuf+26+4, &gyro_[1], 4);
      memcpy(flushbuf+26+8, &gyro_[2], 4);
      memcpy(flushbuf+38, &servo_pos_, 1);
      memcpy(flushbuf+39, wheel_pos_, 2*4);
      memcpy(flushbuf+47, wheel_dt_, 2*4);
      // write the whole 640x480 buffer
      memcpy(flushbuf+55, buf, length);

      struct timeval t1;
      gettimeofday(&t1, NULL);
      float dt = t1.tv_sec - t.tv_sec + (t1.tv_usec - t.tv_usec) * 1e-6;
      if (dt > 0.1) {
        fprintf(stderr, "CameraThread::OnFrame: WARNING: "
            "alloc/copy took %fs\n", dt);
      }

      flush_thread_.AddEntry(output_fd_, flushbuf, flushlen);
      struct timeval t2;
      gettimeofday(&t2, NULL);
      dt = t2.tv_sec - t1.tv_sec + (t2.tv_usec - t1.tv_usec) * 1e-6;
      if (dt > 0.1) {
        fprintf(stderr, "CameraThread::OnFrame: WARNING: "
            "flush_thread.AddEntry took %fs\n", dt);
      }
    }

    {
      static struct timeval t0 = {0, 0};
      float dt = t.tv_sec - t0.tv_sec + (t.tv_usec - t0.tv_usec) * 1e-6;
      if (dt > 0.1 && t0.tv_sec != 0) {
        fprintf(stderr, "CameraThread::OnFrame: WARNING: "
            "%fs gap between frames?!\n", dt);
      }
      t0 = t;
    }

    float dt = t.tv_sec - last_t_.tv_sec + (t.tv_usec - last_t_.tv_usec) * 1e-6;

    if (firstframe_) {
      memcpy(last_encoders_, wheel_pos_, 4*sizeof(uint16_t));
      firstframe_ = false;
      dt = 1.0 / 30.0;
    }
    uint16_t wheel_delta[4];
    for (int i = 0; i < 4; i++) {
      wheel_delta[i] = wheel_pos_[i] - last_encoders_[i];
    }
    memcpy(last_encoders_, wheel_pos_, 4*sizeof(uint16_t));

    // predict using front wheel distance
    float ds = 0.25 * (
            wheel_delta[0] + wheel_delta[1] +
            + wheel_delta[2] + wheel_delta[3]);
    int conesx[10];
    float conestheta[10];
    int ncones = coneslam::FindCones(buf, config_.cone_thresh,
        gyro_[2], 10, conesx, conestheta);

    if (ds > 0) {  // only do coneslam updates while we're moving
      localizer_->Predict(ds, gyro_[2], dt);
      for (int i = 0; i < ncones; i++) {
        localizer_->UpdateLM(conestheta[i], config_.lm_precision * 0.1);
      }
    }

    display_.UpdateConeView(buf, ncones, conesx);
    display_.UpdateEncoders(wheel_pos_);
    {
      coneslam::Particle meanp;
      localizer_->GetLocationEstimate(&meanp);
      float cx, cy, nx, ny, k, t;
      controller_.UpdateLocation(meanp.x, meanp.y, meanp.theta);
      controller_.GetTracker()->GetTarget(meanp.x, meanp.y,
          &cx, &cy, &nx, &ny, &k, &t);

      display_.UpdateParticleView(localizer_, cx, cy, nx, ny);
    }

    float u_a = throttle_ / 127.0;
    float u_s = steering_ / 127.0;
    controller_.UpdateState(config_,
            u_a, u_s,
            accel_, gyro_,
            servo_pos_, wheel_delta,
            dt);
    last_t_ = t;

    if (controller_.GetControl(config_, js_throttle_ / 32767.0,
          js_steering_ / 32767.0, &u_a, &u_s, dt, autodrive_)) {
      steering_ = 127 * u_s;
      throttle_ = 127 * u_a;
      teensy.SetControls(frame_ & 4 ? 1 : 0, throttle_, steering_);
      // pca.SetPWM(PWMCHAN_STEERING, steering_);
      // pca.SetPWM(PWMCHAN_ESC, throttle_);
    }
  }

  bool autodrive_;
  DriveController controller_;
  DriverConfig config_;
  int frame_;

  bool firstframe_;
  uint16_t last_encoders_[4];

 private:
  int output_fd_;
  int frameskip_;
  struct timeval last_t_;
  coneslam::Localizer *localizer_;
};

coneslam::Localizer localizer_(NUM_PARTICLES);
Driver driver_(&localizer_);


static inline float clip(float x, float min, float max) {
  if (x < min) return min;
  if (x > max) return max;
  return x;
}

class DriverInputReceiver : public InputReceiver {
 private:
  static const char *configmenu[];  // initialized below
  static const int N_CONFIGITEMS;

  int config_item_;
  DriverConfig *config_;
  bool x_down_, y_down_;

 public:
  explicit DriverInputReceiver(DriverConfig *config) {
    config_ = config;
    config_item_ = 0;
    x_down_ = y_down_ = false;
    UpdateDisplay();
  }

  virtual void OnDPadPress(char direction) {
    int16_t *value = ((int16_t*) config_) + config_item_;
    switch (direction) {
      case 'U':
        --config_item_;
        if (config_item_ < 0)
          config_item_ = N_CONFIGITEMS - 1;
        fprintf(stderr, "\n");
        break;
      case 'D':
        ++config_item_;
        if (config_item_ >= N_CONFIGITEMS)
          config_item_ = 0;
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

  virtual void OnButtonPress(char button) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    int16_t *value = ((int16_t*) config_) + config_item_;

    switch(button) {
      case '+': // start button: start recording
        if (!driver_.IsRecording()) {
          char fnamebuf[256];
          time_t start_time = time(NULL);
          struct tm start_time_tm;
          localtime_r(&start_time, &start_time_tm);
          strftime(fnamebuf, sizeof(fnamebuf), "cycloid-%Y%m%d-%H%M%S.rec",
              &start_time_tm);
          if (driver_.StartRecording(fnamebuf, 0)) {
            fprintf(stderr, "%d.%06d started recording %s\n",
                tv.tv_sec, tv.tv_usec, fnamebuf);
            display_.UpdateStatus(fnamebuf, 0xffe0);
          }
        }
        break;
      case '-':  // select button: stop recording
        if (driver_.IsRecording()) {
          driver_.StopRecording();
          fprintf(stderr, "%d.%06d stopped recording\n", tv.tv_sec, tv.tv_usec);
          display_.UpdateStatus("recording stopped", 0xffff);
        }
        break;
      case 'H':  // home button: init to start line
        localizer_.Reset();
        display_.UpdateStatus("starting line", 0x07e0);
        break;
      case 'L':
        if (!driver_.autodrive_) {
          fprintf(stderr, "%d.%06d autodrive ON\n", tv.tv_sec, tv.tv_usec);
          driver_.autodrive_ = true;
        }
        break;
      case 'B':
        driver_.controller_.ResetState();
        if (config_->Load()) {
          fprintf(stderr, "config loaded\n");
          int16_t *values = ((int16_t*) config_);
          display_.UpdateConfig(configmenu, N_CONFIGITEMS, config_item_, values);
          display_.UpdateStatus("config loaded", 0xffff);
        }
        fprintf(stderr, "reset kalman filter\n");
        break;
      case 'A':
        if (config_->Save()) {
          fprintf(stderr, "config saved\n");
          display_.UpdateStatus("config saved", 0xffff);
        }
        break;
      case 'X':
        x_down_ = true;
        break;
      case 'Y':
        y_down_ = true;
        break;
    }
  }

  virtual void OnButtonRelease(char button) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    switch (button) {
      case 'L':
        if (driver_.autodrive_) {
          driver_.autodrive_ = false;
          fprintf(stderr, "%d.%06d autodrive OFF\n", tv.tv_sec, tv.tv_usec);
        }
        break;
      case 'X':
        x_down_ = false;
        break;
      case 'Y':
        y_down_ = false;
        break;
    }
  }

  virtual void OnAxisMove(int axis, int16_t value) {
    switch (axis) {
      case 1:  // left stick y axis
        js_throttle_ = -value;
        break;
      case 2:  // right stick x axis
        js_steering_ = value;
        break;
    }
  }

  void UpdateDisplay() {
    // hack because all config values are int16_t's in 1/100th steps
    int16_t *values = ((int16_t*) config_);
    int16_t value = values[config_item_];
    // FIXME: does this work for negative values?
    fprintf(stderr, "%s %d.%02d\r", configmenu[config_item_],
        value / 100, value % 100);
    display_.UpdateConfig(configmenu, N_CONFIGITEMS, config_item_, values);
  }
};

const char *DriverInputReceiver::configmenu[] = {
  "cone thresh",
  "max speed",
  "traction limit",
  "steering kP",
  "steering kD",
  "motor bw",
  "yaw rate bw",
  "cone precision",
};
const int DriverInputReceiver::N_CONFIGITEMS = sizeof(configmenu) / sizeof(configmenu[0]);

int main(int argc, char *argv[]) {
  signal(SIGINT, handle_sigint);

  feenableexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW);

  int fps = 30;

  if (!flush_thread_.Init()) {
    return 1;
  }

#ifdef CAMERA
  if (!Camera::Init(640, 480, fps))
    return 1;
#endif

  JoystickInput js;

  if (!i2c.Open()) {
    fprintf(stderr, "need to enable i2c in raspi-config, probably\n");
    return 1;
  }

  if (!display_.Init()) {
    fprintf(stderr, "run this:\n"
       "sudo modprobe fbtft_device name=adafruit22a rotate=90\n");
    // TODO(asloane): support headless mode
    return 1;
  }

  if (!localizer_.LoadLandmarks("lm.txt")) {
    fprintf(stderr, "if no landmarks yet, just echo 0 >lm.txt and rerun\n");
    return 1;
  }

  bool has_joystick = false;
  if (js.Open()) {
    has_joystick = true;
  } else {
    fprintf(stderr, "joystick not detected, but continuing anyway!\n");
  }

  teensy.Init();
  teensy.SetControls(0, 0, 0);
  teensy.GetFeedback(&servo_pos_, wheel_pos_, wheel_dt_);
  fprintf(stderr, "initial teensy state feedback: \n"
          "  servo %d encoders %d %d %d %d\r",
          servo_pos_, wheel_pos_[0], wheel_pos_[1],
          wheel_pos_[2], wheel_pos_[3]);

  // pca.Init(100);  // 100Hz output
  // pca.SetPWM(PWMCHAN_STEERING, 614);
  // pca.SetPWM(PWMCHAN_ESC, 614);

  imu.Init();

  struct timeval tv;
  gettimeofday(&tv, NULL);
  fprintf(stderr, "%d.%06d camera on @%d fps\n", tv.tv_sec, tv.tv_usec, fps);

  DriverInputReceiver input_receiver(&driver_.config_);
#ifdef CAMERA
  if (!Camera::StartRecord(&driver_)) {
    return 1;
  }

  gettimeofday(&tv, NULL);
  fprintf(stderr, "%d.%06d started camera\n", tv.tv_sec, tv.tv_usec);
#endif

  while (!done) {
    int t = 0, s = 0;
    uint16_t b = 0;
    if (has_joystick && js.ReadInput(&input_receiver)) {
      // nothing to do here
    }
    // FIXME: predict step here?
    {
      float temp;
      imu.ReadIMU(&accel_, &gyro_, &temp);
      // FIXME: imu EKF update step?
      teensy.GetFeedback(&servo_pos_, wheel_pos_, wheel_dt_);
    }
    usleep(1000);
  }

#ifdef CAMERA
  Camera::StopRecord();
#endif
}
