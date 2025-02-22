/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * 
 * All rights reserved.
 * 
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <fam/fam.h>
#include <msg_conversions/msg_conversions.h>
#include <ff_util/ff_names.h>
#include <ff_hw_msgs/PmcCommand.h>

#include <Eigen/QR>

// parameters fam_force_allocation_module_P are set in
//  matlab/code_generation/fam_force_allocation_module_ert_rtw/fam_force_allocation_module_data.c

namespace fam {

Fam::Fam(ros::NodeHandle* nh) : inertia_received_(false) {
  config_.AddFile("gnc.config");
  config_.AddFile("geometry.config");
  ReadParams();
  config_timer_ = nh->createTimer(ros::Duration(1), [this](ros::TimerEvent e) {
      config_.CheckFilesUpdated(std::bind(&Fam::ReadParams, this));}, false, true);

  pt_fam_.Initialize("fam");

  pmc_pub_ = nh->advertise<ff_hw_msgs::PmcCommand>(TOPIC_HARDWARE_PMC_COMMAND, 1);

  // Subscribe to the flight mode to be notified of speed gain command
  flight_mode_sub_ = nh->subscribe(
    TOPIC_MOBILITY_FLIGHT_MODE, 1, &Fam::FlightModeCallback, this);
  inertia_sub_ = nh->subscribe(
    TOPIC_MOBILITY_INERTIA, 1, &Fam::InertiaCallback, this);

  ctl_sub_ = nh->subscribe(TOPIC_GNC_CTL_COMMAND, 5,
    &Fam::CtlCallBack, this, ros::TransportHints().tcpNoDelay());
}

Fam::~Fam() {}

void Fam::CtlCallBack(const ff_msgs::FamCommand & c) {
  ex_time_msg time;
  cmd_msg cmd;
  ctl_msg ctl;

  time.timestamp_sec  = c.header.stamp.sec;
  time.timestamp_nsec = c.header.stamp.nsec;
  msg_conversions::ros_to_array_vector(c.wrench.force, ctl.body_force_cmd);
  msg_conversions::ros_to_array_vector(c.wrench.torque, ctl.body_torque_cmd);
  msg_conversions::ros_to_array_vector(c.accel, ctl.body_accel_cmd);
  msg_conversions::ros_to_array_vector(c.alpha, ctl.body_alpha_cmd);
  msg_conversions::ros_to_array_vector(c.position_error, ctl.pos_err);
  msg_conversions::ros_to_array_vector(c.position_error_integrated, ctl.pos_err_int);
  msg_conversions::ros_to_array_vector(c.attitude_error, ctl.att_err);
  msg_conversions::ros_to_array_vector(c.attitude_error_integrated, ctl.att_err_int);
  ctl.att_err_mag = c.attitude_error_mag;
  ctl.ctl_status  = c.status;
  cmd.cmd_mode    = c.control_mode;

  input_.body_force_cmd =  msg_conversions::ros_to_eigen_vector(c.wrench.force).cast<float>();
  input_.body_torque_cmd =  msg_conversions::ros_to_eigen_vector(c.wrench.torque).cast<float>();

  Step(&time, &cmd, &ctl);
}

void Fam::FlightModeCallback(const ff_msgs::FlightMode::ConstPtr& mode) {
  std::lock_guard<std::mutex> lock(mutex_speed_);
  speed_ = mode->speed;
  input_.speed_gain_cmd = mode->speed;
}

void Fam::InertiaCallback(const geometry_msgs::InertiaStamped::ConstPtr& inertia) {
  std::lock_guard<std::mutex> lock(mutex_mass_);
  center_of_mass_ = msg_conversions::ros_to_eigen_vector(inertia->inertia.com).cast<float>();
  fam_.UpdateCOM(center_of_mass_);
  inertia_received_ = true;
}

void Fam::TestTwoVectors(const char* name, const Eigen::Matrix<float, 6, 1> new_array,
                                   const float old_array[], float tolerance) {
  for (int i = 0; i < 6; i++) {
    float difference = old_array[i] - new_array[i];
    float perc_difference = difference / old_array[i];
    if (fabs(difference) > tolerance) {
      std::string p1, p2;
      for (int i = 0; i < 6; i++) {
        p1 += " " + std::to_string(new_array[i]);
        p2 += " " + std::to_string(old_array[i]);
      }
      ROS_ERROR("%s New: %s, Old: %s", name, p1.c_str(), p2.c_str());
    }
  }
}

void Fam::Step(ex_time_msg* ex_time, cmd_msg* cmd, ctl_msg* ctl) {
  {
    std::lock_guard<std::mutex> lock(mutex_speed_);
    // Overwrite the speed command with the cached value, provided
    // through the flight mode message offered by the choreographer
    cmd->speed_gain_cmd = speed_;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_mass_);
    if (!inertia_received_) {
      ROS_DEBUG_STREAM_THROTTLE(10, "FAM step waiting for inertia.");
      return;
    }
    gnc_.cmc_.center_of_mass[0] = center_of_mass_[0];
    gnc_.cmc_.center_of_mass[1] = center_of_mass_[1];
    gnc_.cmc_.center_of_mass[2] = center_of_mass_[2];
  }

  // Step the FAM simulink code
  pt_fam_.Tick();
  uint8_t speed_cmd[2];
  Eigen::Matrix<float, 12, 1> servo_pwm_cmd;
  if (compare_fam_ || !use_old_fam_)
    fam_.Step(input_, speed_cmd, servo_pwm_cmd);
  if (compare_fam_ || use_old_fam_)
    gnc_.Step(ex_time, cmd, ctl);
  pt_fam_.Tock();

  if (compare_fam_) {
    if (speed_cmd[0] != gnc_.act_.act_impeller_speed_cmd[0] || speed_cmd[1] != gnc_.act_.act_impeller_speed_cmd[1])
      ROS_ERROR("Speed incorrect. New: %d %d Old: %d %d", speed_cmd[0], speed_cmd[1],
                 gnc_.act_.act_impeller_speed_cmd[0], gnc_.act_.act_impeller_speed_cmd[1]);
    TestTwoVectors("PMC1", servo_pwm_cmd.head<6>(), gnc_.act_.act_servo_pwm_cmd, 0.1);
    TestTwoVectors("PMC2", servo_pwm_cmd.tail<6>(), gnc_.act_.act_servo_pwm_cmd + 6, 0.1);
  }

  // Send the PMC command
  static ff_hw_msgs::PmcCommand pmc;
  pmc.header.stamp = ros::Time::now();
  pmc.header.frame_id = "body";
  pmc.goals.resize(2);
  if (use_old_fam_) {
    pmc.goals[0].motor_speed = gnc_.act_.act_impeller_speed_cmd[0];
    pmc.goals[1].motor_speed = gnc_.act_.act_impeller_speed_cmd[1];
    std::copy(gnc_.act_.act_servo_pwm_cmd, gnc_.act_.act_servo_pwm_cmd + 6,
        pmc.goals[0].nozzle_positions.c_array());
    std::copy(gnc_.act_.act_servo_pwm_cmd + 6, gnc_.act_.act_servo_pwm_cmd + 12,
        pmc.goals[1].nozzle_positions.c_array());
  } else {
    pmc.goals[0].motor_speed = speed_cmd[0];
    pmc.goals[1].motor_speed = speed_cmd[1];
    for (int i = 0; i < 6; i++) {
      pmc.goals[0].nozzle_positions[i] = (unsigned char)servo_pwm_cmd[i];
      pmc.goals[1].nozzle_positions[i] = (unsigned char)servo_pwm_cmd[6 + i];
    }
  }
  pmc_pub_.publish<ff_hw_msgs::PmcCommand>(pmc);

  pt_fam_.Send();
}

void Fam::ReadParams(void) {
  if (!config_.ReadFiles()) {
    ROS_ERROR("Failed to read config files.");
    return;
  }
  if (!config_.GetBool("fam_use_old", &use_old_fam_))
    ROS_FATAL("fam_use_old not specified.");
  if (!config_.GetBool("fam_compare_old", &compare_fam_))
    ROS_FATAL("fam_compare_old not specified.");
  gnc_.ReadParams(&config_);
}

}  // end namespace fam
