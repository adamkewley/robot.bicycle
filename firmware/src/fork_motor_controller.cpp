#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include "constants.h"
#include "fork_motor_controller.h"
#include "MPU6050.h"
#include "SystemState.h"

namespace hardware {

const uint8_t ccr_channel = 2;                 // PWM Channel 2
const float max_current = 6.0f;                // Copley Controls ACJ-055-18
const float torque_constant = 106.459f * constants::Nm_per_ozfin;
const float max_steer_angle = 45.0f * constants::rad_per_degree;

ForkMotorController::ForkMotorController()
  : MotorController("Fork"),
  e_(STM32_TIM3, constants::fork_counts_per_revolution),
  m_(GPIOF, GPIOF_STEER_DIR, GPIOF_STEER_ENABLE, GPIOF_STEER_FAULT,
     STM32_TIM1, ccr_channel, max_current, torque_constant),
  derivative_filter_{0, 10*2*constants::pi,
                     10*2*constants::pi, constants::loop_period_s},
  yaw_rate_command_{0.0f},
  x_pi_{0.0f},
  estimation_threshold_{-0.75f / constants::wheel_radius},
  estimation_triggered_{false},
  control_triggered_{false},
  control_delay_{10u}
{
  instances[fork] = this;
}

ForkMotorController::~ForkMotorController()
{
  instances[fork] = 0;
}

void ForkMotorController::set_reference(float yaw_rate)
{
  yaw_rate_command_ = yaw_rate;
}

void ForkMotorController::set_estimation_threshold(float speed)
{
  estimation_threshold_  = speed / -constants::wheel_radius;
}

void ForkMotorController::set_control_delay(uint32_t N)
{
  control_delay_= N;
}

void ForkMotorController::disable()
{
  m_.disable();
}

void ForkMotorController::enable()
{
  m_.enable();
}

void ForkMotorController::set_estimation_threshold_shell(BaseSequentialStream *chp,
                                                         int argc, char *argv[])
{
  if (argc == 1) {
      ForkMotorController* fmc = reinterpret_cast<ForkMotorController*>(instances[fork]);
      if (fmc) {
        fmc->set_estimation_threshold(tofloat(argv[0]));
        chprintf(chp, "%s estimation threshold set to %f.\r\n", fmc->name(),
                 fmc->estimation_threshold_);
      } else {
        chprintf(chp, "Enable collection before setting estimation threshold.\r\n");
      }
  } else {
    chprintf(chp, "Invalid usage.\r\n");
  }
}

void ForkMotorController::set_control_delay_shell(BaseSequentialStream *chp,
                                                  int argc, char *argv[])
{
  if (argc == 1) {
      ForkMotorController* fmc = reinterpret_cast<ForkMotorController*>(instances[fork]);
      if (fmc) {
        uint32_t N = std::atoi(argv[0]);
        fmc->set_control_delay(N);
        chprintf(chp, "%s control delay set to begin %u samples after estimation.\r\n", fmc->name(),
                 fmc->control_delay_);
      } else {
        chprintf(chp, "Enable collection before setting control threshold.\r\n");
      }
  } else {
    chprintf(chp, "Invalid usage.\r\n");
  }
}

void ForkMotorController::set_thresholds_shell(BaseSequentialStream *chp,
                                               int argc, char *argv[]) { if (argc == 2) {
      ForkMotorController* fmc = reinterpret_cast<ForkMotorController*>(instances[fork]);
      fmc->set_estimation_threshold(tofloat(argv[0]));
      uint32_t N = std::atoi(argv[0]);
      fmc->set_control_delay(N);
      chprintf(chp, "%s estimation threshold set to %f.\r\n", fmc->name(),
               fmc->estimation_threshold_);
      chprintf(chp, "%s control delay set to begin %u samples after estimation.\r\n", fmc->name(),
               fmc->control_delay_);
  } else {
    chprintf(chp, "Invalid usage.\r\n");
  }
}

void ForkMotorController::update(Sample & s)
{
  s.encoder.steer = e_.get_angle();
  s.encoder.steer_rate = derivative_filter_.output(s.encoder.steer);
  derivative_filter_.update(s.encoder.steer); // update for next iteration
  set_gyro_lean(s);

  s.set_point.psi_dot = yaw_rate_command_;
  s.yaw_rate_pi.e = s.set_point.psi_dot - MPU6050::psi_dot(s);
  x_pi_ += s.yaw_rate_pi.e;
  s.yaw_rate_pi.x = x_pi_;

  s.motor_torque.desired_steer = 0.0f;
  if (should_estimate(s) && fork_control_.set_sample(s)) {
    const float torque = fork_control_.compute_updated_torque(m_.get_torque());
    if (should_control(s)) {
      s.motor_torque.desired_steer = torque;
      m_.set_torque(torque);
    } else {
      m_.set_torque(0.0f);
    }
  } else {
    m_.set_torque(0.0f);
  }
  s.motor_torque.steer = m_.get_torque();

  if (e_.rotation_direction())
    s.system_state |= systemstate::SteerEncoderDir;
  if (m_.is_enabled())
    s.system_state |= systemstate::SteerMotorEnable;
  if (m_.has_fault())
    s.system_state |= systemstate::SteerMotorFault;
  if (m_.current_direction())
    s.system_state |= systemstate::SteerMotorCurrentDir;
  s.threshold.estimation = estimation_threshold_;
  s.threshold.control = 0.0f; //  control_threshold_;
  s.has_threshold = true;
}

// estimation/control thresholds are in terms of wheel rate, which is defined
// to be negative when the speed of the bicycle is positive. estimation/control
// should occur when speed > threshold which is equivalent to rate < threshold.
bool ForkMotorController::should_estimate(const Sample& s)
{
  if (!estimation_triggered_)
    estimation_triggered_ = s.encoder.rear_wheel_rate < estimation_threshold_;
    fork_control_.set_state(s.gyro_lean.angle, s.encoder.steer,
                            s.mpu6050.gyroscope_y, s.encoder.steer_rate);
  return estimation_triggered_;
}

bool ForkMotorController::should_control(const Sample& s)
{
  if (!control_triggered_)
    control_triggered_ = --control_delay_ == 0;
  return control_triggered_ && std::abs(s.encoder.steer) < max_steer_angle;
}

void ForkMotorController::set_gyro_lean(Sample& s)
{
  s.has_gyro_lean = true;
  static std::array<float, 100> lean_array {{}};
  static int lean_i = 0;
  static uint32_t system_time_prev = 0;

  // first pass, use static lean value
  if (lean_array[lean_i] == 0.0f) {
    float ax = s.mpu6050.accelerometer_x;
    float ay = s.mpu6050.accelerometer_y;
    float az = s.mpu6050.accelerometer_z;
    float accel_mag = std::sqrt(ax*ax + ay*ay + az*az);
    float lean_static = std::asin(ax / accel_mag);

    lean_array[lean_i] = lean_static;
    lean_i = (lean_i + 1)  % lean_array.size();
    system_time_prev = s.system_time;

    // after lean_array.size() samples, set the average value
    if (lean_i == 0 && lean_array[0] != 0.0f) {
      float lean_avg = (std::accumulate(lean_array.begin(),
                                        lean_array.end(), 0.0f) /
                        lean_array.size());
      lean_array[0] = lean_avg;
      s.gyro_lean.angle = lean_avg;
    } else {
      s.gyro_lean.angle = lean_static;
    }
    s.gyro_lean.startup = true;
    return;
  }

  // after setting average static lean, use gyro lean
  float gyro_lean;
  float dt = ((s.system_time - system_time_prev) *
              constants::system_timer_seconds_per_count);
  gyro_lean = lean_array[0] + s.mpu6050.gyroscope_y * dt;

  system_time_prev = s.system_time;
  lean_array[0] = gyro_lean;
  s.gyro_lean.startup = false;
  s.gyro_lean.angle = gyro_lean;
  return;
}

} // namespace hardware
