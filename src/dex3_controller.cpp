#include "rclcpp/rclcpp.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>

#include <unitree_hg/msg/hand_cmd.hpp>
#include <unitree_hg/msg/motor_cmd.hpp>

#include <unitree_hg/msg/hand_state.hpp>
#include <unitree_hg/msg/motor_state.hpp>
#include <unitree_hg/msg/press_sensor_state.hpp>

#include <std_msgs/msg/bool.hpp>

#include <vector>
#include <chrono>
#include <map>
#include <string>
#include <urdf/model.h>

#include <g1_dex3_joint_defs.hpp>

using namespace std::chrono_literals;

typedef struct {
    uint8_t id     : 4;   // Motor ID: 0,1...,13,14  15 represents broadcasting to all motors
    uint8_t status : 3;   // Working mode: 0.Lock 1.FOC 6
    uint8_t timeout: 1;   // Master->Motor: 0.Disable timeout protection 1.Enable (Default 1s timeout)
                          // Motor->Master: 0.No timeout 1.Timeout protection triggered (needs control bit 0 to clear)
} RIS_Mode_t;             // Control mode 1Byte

// Struct to hold joint limits
struct JointLimits {
  double lower;
  double upper;
  double velocity;
  double effort;
};

class Dex3Controller : public rclcpp::Node {
public:
  Dex3Controller() : Node("dex3_controller") {
    // Parameterize side, then input/output topics (defaulting to side-based names)
    this->declare_parameter("side", "left");
    this->get_parameter("side", side);
    input_topic = "/dex3/" + side + "/command";
    output_topic = "/dex3/" + side + "/cmd";
    this->declare_parameter("input_topic", input_topic);
    this->get_parameter("input_topic", input_topic);
    state_topic = "/lf/dex3/" + side + "/state";
    this->declare_parameter("tactile_threshold", tactile_threshold_);
    this->get_parameter("tactile_threshold", tactile_threshold_);

    rclcpp::QoS qos_profile(10);
    qos_profile.best_effort();

    hand_cmd_pub_ = this->create_publisher<unitree_hg::msg::HandCmd>(output_topic, qos_profile);
    // Subscribe to high-level commands (e.g., "open hand", "close hand")
    hand_cmd_sub_ = this->create_subscription<std_msgs::msg::Bool>(
      input_topic, 10,
      std::bind(&Dex3Controller::handCmdCallback, this, std::placeholders::_1));

    // Subscribe to feedback from the hand
    hand_state_sub_ = this->create_subscription<unitree_hg::msg::HandState>(
      state_topic, 10,
      std::bind(&Dex3Controller::handStateCallback, this, std::placeholders::_1));

    // Load URDF from /robot_description parameter and parse joint limits
    std::string urdf_xml;
    auto client = this->create_client<rcl_interfaces::srv::GetParameters>("/robot_state_publisher/get_parameters");
    int wait_attempts = 0;
    while (!client->wait_for_service(std::chrono::milliseconds(200))) {
      if (++wait_attempts > 25) { // Wait up to 5 seconds, then give up
        RCLCPP_ERROR(this->get_logger(), "Timeout waiting for /robot_state_publisher service. Node will exit.");
        rclcpp::shutdown();
        return;
      }
      RCLCPP_WARN_THROTTLE(this->get_logger(), *this->get_clock(), 2000, "Waiting for /robot_state_publisher service...");
    }
    
    auto request = std::make_shared<rcl_interfaces::srv::GetParameters::Request>();
    request->names.push_back("robot_description");

    auto future = client->async_send_request(request);
    if (rclcpp::spin_until_future_complete(this->get_node_base_interface(), future) == rclcpp::FutureReturnCode::SUCCESS) {
      auto response = future.get();
      if (response->values.size() == 1 && response->values[0].type == rcl_interfaces::msg::ParameterType::PARAMETER_STRING) {
        urdf_xml = response->values[0].string_value;
      } else {
        RCLCPP_FATAL(this->get_logger(), "robot_description not found in /robot_state_publisher");
        rclcpp::shutdown();
        return;
      }
    } else {
      RCLCPP_FATAL(this->get_logger(), "Failed to connect to /robot_state_publisher/get_parameters service");
      rclcpp::shutdown();
      return;
    }

    if (urdf_xml.empty()) {
      RCLCPP_ERROR(this->get_logger(), "robot_description parameter is missing or empty. Cannot continue.");
      rclcpp::shutdown();
      return;
    }
    urdf::Model model;
    if (!model.initString(urdf_xml)) {
      RCLCPP_ERROR(this->get_logger(), "Failed to parse URDF from robot_description parameter");
      rclcpp::shutdown();
      return;
    }
    // Extract hand joints for the specified side (using <side>_hand_palm_link as the base link)
    std::string hand_base_link = side + "_hand_palm_link";
    std::set<std::string> hand_joints_set;
    std::function<void(const std::string&)> collect_hand_joints;
    collect_hand_joints = [&](const std::string& link_name) {
      RCLCPP_INFO(this->get_logger(), "Collecting hand joints for link: %s", link_name.c_str());
      for (const auto& joint_pair : model.joints_) {
        auto joint = joint_pair.second;
        if (joint && joint->parent_link_name == link_name) {
          hand_joints_set.insert(joint->name);
          // Recursively collect child joints
          if (!joint->child_link_name.empty()) {
            collect_hand_joints(joint->child_link_name);
          }
        }
      }
    };
    collect_hand_joints(hand_base_link);
    hand_joint_names.clear();
    joint_limits_.clear();
    for (const auto& joint_name : hand_joints_set) {
      RCLCPP_INFO(this->get_logger(), "Found hand joint: %s", joint_name.c_str());
      hand_joint_names.push_back(joint_name);
      auto joint = model.getJoint(joint_name);
      if (joint && joint->limits) {
        RCLCPP_INFO(this->get_logger(), "Hand joint %s limits: lower = %f, upper = %f, velocity = %f, effort = %f",
                    joint_name.c_str(), joint->limits->lower, joint->limits->upper,
                    joint->limits->velocity, joint->limits->effort);
        joint_limits_[joint_name] = {joint->limits->lower, joint->limits->upper, joint->limits->velocity, joint->limits->effort};
      }
    }
    RCLCPP_INFO(this->get_logger(), "Loaded %zu hand joints for side '%s' (base link: %s) from robot_description", hand_joint_names.size(), side.c_str(), hand_base_link.c_str());

    RCLCPP_INFO(this->get_logger(), "Dex3Controller started. Subscribing to %s, publishing to %s, feedback from %s", input_topic.c_str(), output_topic.c_str(), state_topic.c_str());

    // In constructor, after all publishers/subscribers are created:
    calibration_timer_ = this->create_wall_timer(
        std::chrono::milliseconds(500),  // Wait 0.5s after startup
        [this]() {
            static bool done = false;
            if (!done) {
                this->rotateMotorsCalibration();
                done = true;
            }
        }
    );

    // Timer for periodic closed-loop grasping (10 Hz)
    closed_loop_timer_ = this->create_wall_timer(
      100ms, std::bind(&Dex3Controller::closedLoopGrasping, this));
  }

  ~Dex3Controller() override {
    RCLCPP_INFO(this->get_logger(), "Releasing hand on shutdown...");

    unitree_hg::msg::HandCmd release_cmd;
    release_cmd.motor_cmd.resize(hand_joint_names.size());
    for (size_t i = 0; i < hand_joint_names.size(); ++i) {
      RIS_Mode_t ris_mode;
      ris_mode.id = i;
      ris_mode.status = 0x00;  // Lock mode
      ris_mode.timeout = 0x01; // Enable timeout protection

      uint8_t mode = 0;
      mode |= (ris_mode.id & 0x0F);
      mode |= (ris_mode.status & 0x07) << 4;
      mode |= (ris_mode.timeout & 0x01) << 7;

      release_cmd.motor_cmd[i].mode = mode;
      release_cmd.motor_cmd[i].q = 0.f;
      release_cmd.motor_cmd[i].dq = 0.f;
      release_cmd.motor_cmd[i].kp = 0.f;
      release_cmd.motor_cmd[i].kd = 0.f;
      release_cmd.motor_cmd[i].tau = 0.f;
    }
    hand_cmd_pub_->publish(release_cmd);
    rclcpp::sleep_for(std::chrono::milliseconds(10));
  }

private:
  std::string side;
  std::string input_topic;
  std::string output_topic;
  std::string state_topic;
  double tactile_threshold_ = 0.1;

  void handCmdCallback(const std_msgs::msg::Bool::SharedPtr msg) {
    if (!msg->data) {
      RCLCPP_INFO(this->get_logger(), "Received open hand command");
      closing_ = false; // Interrupt any closing loop
      tactile_calibration_needed_ = true; // Trigger tactile calibration on next reading

      unitree_hg::msg::HandCmd hand_cmd;
      hand_cmd.motor_cmd.resize(hand_joint_names.size()); // Resize to number of hand joints
      for (size_t i = 0; i < hand_joint_names.size(); ++i) {
        auto joint_name = hand_joint_names[i];
        auto target_index = hand_joint_name_to_index.at(joint_name);

        RIS_Mode_t ris_mode;
        ris_mode.id = target_index; // Set id
        ris_mode.status = 0x01;  // Set status to 0x01 (FOC mode)
        ris_mode.timeout = 0x00; // Set timeout to 0x00 (no timeout)
    
        uint8_t mode = 0;
        mode |= (ris_mode.id & 0x0F); // Get lower 4 bits of id
        mode |= (ris_mode.status & 0x07) << 4; // Get upper 3 bits of status and shift left 4 bits
        mode |= (ris_mode.timeout & 0x01) << 7; // Get upper 1 bit of timeout and shift left 7 bits

        hand_cmd.motor_cmd[target_index].mode = mode; // Set the mode for the hand joint

        float target_position = 0.0f; // Default target position for opening the hand

        // Clamp using URDF joint limits if available
        auto lim = joint_limits_.at(joint_name);

        //left hand thumb_1 should be set to the lower limit
        //right hand thumb_1 should be set to the upper limit
        //for other fingers, its always 0
        if (joint_name.find("thumb_1") != std::string::npos || joint_name.find("thumb_2") != std::string::npos) {
          if (side == "left") {
            target_position = lim.lower;
          } else if (side == "right") {
            target_position = lim.upper;
          }
        } else if (joint_name.find("thumb_0") != std::string::npos) {
          target_position = (lim.lower + lim.upper) / 2.0f;
        } else {
          if (side == "left") {
            target_position = lim.upper;
          } else if (side == "right") {
            target_position = lim.lower;
          }
        }
        

        hand_cmd.motor_cmd[target_index].q = target_position; // Open the hand fully (or thumb_0 to middle)
        hand_cmd.motor_cmd[target_index].dq = 0.f; // No velocity command for opening
        hand_cmd.motor_cmd[target_index].kp = 0.5f;
        hand_cmd.motor_cmd[target_index].kd = 0.1f;
        hand_cmd.motor_cmd[target_index].tau = 0.f;

        RCLCPP_INFO(this->get_logger(), "Setting hand joint %s to position %f", joint_name.c_str(), target_position);
      }

      // Publish the hand command to open it fully
      hand_cmd_pub_->publish(hand_cmd);
      rclcpp::sleep_for(1s);  // Wait for the last command to take effect
    } else {
      RCLCPP_INFO(this->get_logger(), "Received close hand command");
      if (!closing_) {
        closing_ = true;

        // No need to start a timer, main loop handles closing
      }
    }
  }

  void handStateCallback(const unitree_hg::msg::HandState::SharedPtr msg) {
    RCLCPP_DEBUG(this->get_logger(), "Received hand state feedback");
    // Tactile calibration step
    if (tactile_calibration_needed_) {
      size_t max_idx = 0;
      for (const auto& press : msg->press_sensor_state) {
        if (!press.pressure.empty()) {
          max_idx = std::max(max_idx, press.pressure.size() - 1);
        }
      }
      if (tactile_baseline_.size() <= max_idx) tactile_baseline_.resize(max_idx + 1, std::numeric_limits<float>::quiet_NaN());
      if (valid_tactile_indices_.size() <= max_idx) valid_tactile_indices_.resize(max_idx + 1, false);
      // Set baseline only for valid sensors
      for (const auto& press : msg->press_sensor_state) {
        for (size_t idx = 0; idx < press.pressure.size(); ++idx) {
          if (press.pressure[idx] != 30000 && std::isnan(tactile_baseline_[idx])) {
            tactile_baseline_[idx] = press.pressure[idx] / 10000.0f;
            valid_tactile_indices_[idx] = true;
          }
        }
      }
      // Calibration is done after first pass, only for valid indices
      tactile_calibration_needed_ = false;
      RCLCPP_INFO(this->get_logger(), "Tactile sensors calibrated. Baseline set for valid indices only.");
    }
    // Aggregate tactile sensor values for thumb, index, middle, and palm, using only valid values and scaling
    float thumb_sum = 0.0f, index_sum = 0.0f, middle_sum = 0.0f, palm_sum = 0.0f;
    size_t thumb_count = 0, index_count = 0, middle_count = 0, palm_count = 0;
    for (const auto& press : msg->press_sensor_state) {
      // Thumb: indices 0, 1
      for (size_t idx : {0, 1}) {
        if (idx < press.pressure.size() && press.pressure[idx] != 30000 && valid_tactile_indices_.size() > idx && valid_tactile_indices_[idx]) {
          float val = press.pressure[idx] / 10000.0f;
          if (tactile_baseline_.size() > idx) val -= tactile_baseline_[idx];
          thumb_sum += val;
          ++thumb_count;
        }
      }
      // Index: indices 4, 5
      for (size_t idx : {4, 5}) {
        if (idx < press.pressure.size() && press.pressure[idx] != 30000 && valid_tactile_indices_.size() > idx && valid_tactile_indices_[idx]) {
          float val = press.pressure[idx] / 10000.0f;
          if (tactile_baseline_.size() > idx) val -= tactile_baseline_[idx];
          index_sum += val;
          ++index_count;
        }
      }
      // Middle: indices 2, 3
      for (size_t idx : {2, 3}) {
        if (idx < press.pressure.size() && press.pressure[idx] != 30000 && valid_tactile_indices_.size() > idx && valid_tactile_indices_[idx]) {
          float val = press.pressure[idx] / 10000.0f;
          if (tactile_baseline_.size() > idx) val -= tactile_baseline_[idx];
          middle_sum += val;
          ++middle_count;
        }
      }
      // Palm: indices 6, 7, 8
      for (size_t idx : {6, 7, 8}) {
        if (idx < press.pressure.size() && press.pressure[idx] != 30000 && valid_tactile_indices_.size() > idx && valid_tactile_indices_[idx]) {
          float val = press.pressure[idx] / 10000.0f;
          if (tactile_baseline_.size() > idx) val -= tactile_baseline_[idx];
          palm_sum += val;
          ++palm_count;
        }
      }
    }
    float thumb_avg = thumb_count > 0 ? thumb_sum / thumb_count : 0.0f;
    float index_avg = index_count > 0 ? index_sum / index_count : 0.0f;
    float middle_avg = middle_count > 0 ? middle_sum / middle_count : 0.0f;
    float palm_avg = palm_count > 0 ? palm_sum / palm_count : 0.0f;
    float finger_palm_sum = 0.0f;
    int finger_palm_count = 0;
    if (index_count > 0) { finger_palm_sum += index_sum; finger_palm_count += index_count; }
    if (middle_count > 0) { finger_palm_sum += middle_sum; finger_palm_count += middle_count; }
    if (palm_count > 0) { finger_palm_sum += palm_sum; finger_palm_count += palm_count; }
    float finger_palm_avg = finger_palm_count > 0 ? finger_palm_sum / finger_palm_count : 0.0f;
    thumb_tactile_ = thumb_avg;
    index_tactile_ = index_avg;
    middle_tactile_ = middle_avg;
    palm_tactile_ = palm_avg;
    finger_tactile_ = finger_palm_avg;
    RCLCPP_DEBUG(this->get_logger(), "Tactile thumb avg: %f, index avg: %f, middle avg: %f, palm avg: %f, finger/palm avg: %f", thumb_avg, index_avg, middle_avg, palm_avg, finger_palm_avg);

    // Update current_positions_ from feedback
    current_positions_.resize(hand_joint_names.size(), 0.0f);
    if (msg->motor_state.size() == hand_joint_names.size()) {
      for (size_t i = 0; i < hand_joint_names.size(); ++i) {
        auto joint_name = hand_joint_names[i];
        auto target_index = hand_joint_name_to_index.at(joint_name);

        current_positions_[target_index] = msg->motor_state[target_index].q;
      }
    }
  }

  void closedLoopGrasping() {
    static std::vector<float> open_positions, closed_positions;
    static bool initialized = false;
    if (closing_) {
      // Initialize joint positions if not done
      if (!initialized) {
        size_t n = hand_joint_names.size();
        open_positions.resize(n, 0.0f);
        closed_positions.resize(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
          const auto& joint_name = hand_joint_names[i];
          auto target_index = hand_joint_name_to_index.at(joint_name);

          auto lim = joint_limits_.at(joint_name);

          if (joint_name.find("thumb_0") != std::string::npos) {
            open_positions[target_index] = 0.0f;
            closed_positions[target_index] = 0.0f;
          } else if (joint_name.find("thumb_1") != std::string::npos || joint_name.find("thumb_2") != std::string::npos) {
            if (side == "left") {
              open_positions[target_index] = lim.lower;
              closed_positions[target_index] = lim.upper;
            } else if (side == "right") {
              open_positions[target_index] = lim.upper;
              closed_positions[target_index] = lim.lower;
            }
          } else {
            if (side == "left") {
              open_positions[target_index] = 0.f;
              closed_positions[target_index] = lim.lower;
            } else if (side == "right") {
              open_positions[target_index] = 0.f;
              closed_positions[target_index] = lim.upper;
            }
          }
        }
        initialized = true;
      }
      // Feedback-driven grasp maintenance
      const float max_delta = 5.0f;
      double thumb_val = thumb_tactile_;
      double index_val = 0.0, middle_val = 0.0, palm_val = 0.0;
      // Calculate index, middle, palm averages from the last handStateCallback
      // We'll need to store these as member variables, so add them:
      // double index_tactile_ = 0.0, middle_tactile_ = 0.0, palm_tactile_ = 0.0;
      // Set them in handStateCallback when calculating thumb_tactile_ and finger_tactile_
      index_val = index_tactile_;
      middle_val = middle_tactile_;
      palm_val = palm_tactile_;
      // New need_regrip logic
      bool thumb_ok = thumb_val > tactile_threshold_;
      bool index_ok = index_val > tactile_threshold_;
      bool middle_ok = middle_val > tactile_threshold_;
      bool palm_ok = palm_val > tactile_threshold_;
      bool need_regrip = true;
      if ((thumb_ok && (index_ok || middle_ok || palm_ok)) ||
          (index_ok && (palm_ok || thumb_ok)) ||
          (middle_ok && (palm_ok || thumb_ok))) {
        need_regrip = false;
      }
      RCLCPP_INFO(this->get_logger(), "Tactile: thumb=%f, index=%f, middle=%f, palm=%f, Need regrip: %s", thumb_val, index_val, middle_val, palm_val, need_regrip ? "true" : "false");
      if (need_regrip && current_positions_.size() == closed_positions.size()) {
        unitree_hg::msg::HandCmd interp_cmd;
        interp_cmd.motor_cmd.resize(hand_joint_names.size());
        for (size_t i = 0; i < hand_joint_names.size(); ++i) {
          const auto& joint_name = hand_joint_names[i];
          auto target_index = hand_joint_name_to_index.at(joint_name);

          RIS_Mode_t ris_mode;
          ris_mode.id = target_index;
          ris_mode.status = 0x01;
          ris_mode.timeout = 0x00;

          uint8_t mode = 0;
          mode |= (ris_mode.id & 0x0F);
          mode |= (ris_mode.status & 0x07) << 4;
          mode |= (ris_mode.timeout & 0x01) << 7;

          interp_cmd.motor_cmd[target_index].mode = mode;

          float target = closed_positions[target_index];
          float diff = target - current_positions_[target_index];
          float step = std::clamp(diff, -max_delta, max_delta); // Limit step size to max_delta
          float next = current_positions_[target_index] + step;

          interp_cmd.motor_cmd[target_index].q = next;
          interp_cmd.motor_cmd[target_index].dq = 0.f;
          interp_cmd.motor_cmd[target_index].kp = 0.5f;
          interp_cmd.motor_cmd[target_index].kd = 0.1f;
          interp_cmd.motor_cmd[target_index].tau = 0.f;

          RCLCPP_INFO(this->get_logger(), "Interpolating hand joint %s to position %f (current: %f, target: %f)", joint_name.c_str(), next, current_positions_[target_index], target);
        }
        hand_cmd_pub_->publish(interp_cmd);
      }
    } else {
      initialized = false;
    }
  }

  void rotateMotorsCalibration() {
    // This function will be called once at startup to sweep all motors through their range
    RCLCPP_INFO(this->get_logger(), "Starting hand joint discovery sweep... %zu joints found", hand_joint_names.size());
    std::vector<float> maxLimits(hand_joint_names.size(), 0.0f);
    std::vector<float> minLimits(hand_joint_names.size(), 0.0f);
    for (size_t i = 0; i < hand_joint_names.size(); ++i) {
      const auto& joint_name = hand_joint_names[i];
      auto target_index = hand_joint_name_to_index.at(joint_name);

      auto lim = joint_limits_.at(joint_name);

      minLimits[target_index] = lim.lower;
      maxLimits[target_index] = lim.upper;
      RCLCPP_INFO(this->get_logger(), "Hand joint %s limits %zu: min = [%s], max = [%s]", joint_name.c_str(), target_index,
      std::to_string(minLimits[target_index]).c_str(), std::to_string(maxLimits[target_index]).c_str());
    }

    int steps = 400; // Number of steps for a full sweep
    for (int count = 0; count < steps; ++count) {
      unitree_hg::msg::HandCmd msg;
      msg.motor_cmd.resize(hand_joint_names.size());
      for (size_t i = 0; i < hand_joint_names.size(); ++i) {
        const auto& joint_name = hand_joint_names[i];
        auto target_index = hand_joint_name_to_index.at(joint_name);

        RIS_Mode_t ris_mode;
        ris_mode.id = target_index;
        ris_mode.status = 0x01;
        ris_mode.timeout = 0x00;
        uint8_t mode = 0;
        mode |= (ris_mode.id & 0x0F);
        mode |= (ris_mode.status & 0x07) << 4;
        mode |= (ris_mode.timeout & 0x01) << 7;
        msg.motor_cmd[target_index].mode = mode;
        msg.motor_cmd[target_index].tau = 0;
        msg.motor_cmd[target_index].kp = 0.5f;
        msg.motor_cmd[target_index].kd = 0.1f;
        float range = maxLimits[target_index] - minLimits[target_index];
        float mid = (maxLimits[target_index] + minLimits[target_index]) / 2.0f;
        float amplitude = range / 2.0f;
        float q = mid + amplitude * std::sin(count / static_cast<float>(steps) * M_PI * 2.0f);
        msg.motor_cmd[target_index].q = q;
      }
      hand_cmd_pub_->publish(msg);
      RCLCPP_DEBUG(this->get_logger(), "Sweeping hand joints: step %d/%d", count + 1, steps);
      rclcpp::sleep_for(std::chrono::milliseconds(10));
    }
    RCLCPP_INFO(this->get_logger(), "Hand joint discovery sweep complete.");
  }

  std::vector<std::string> hand_joint_names;
  bool closing_ = false;
  rclcpp::Publisher<unitree_hg::msg::HandCmd>::SharedPtr hand_cmd_pub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr hand_cmd_sub_;
  rclcpp::Subscription<unitree_hg::msg::HandState>::SharedPtr hand_state_sub_;
  std::map<std::string, JointLimits> joint_limits_;

  double thumb_tactile_ = 0.0;
  double index_tactile_ = 0.0;
  double middle_tactile_ = 0.0;
  double palm_tactile_ = 0.0;
  double finger_tactile_ = 0.0;

  rclcpp::TimerBase::SharedPtr calibration_timer_;
  rclcpp::TimerBase::SharedPtr closed_loop_timer_;

  std::vector<float> current_positions_;
  std::vector<float> tactile_baseline_;
  std::vector<bool> valid_tactile_indices_;
  bool tactile_calibration_needed_ = false;
};

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<Dex3Controller>());
  rclcpp::shutdown();
  return 0;
}

