#include "kacanopen_hardware.h"

namespace kaco
{
KaCanopenHardware::KaCanopenHardware(Master* master, ros::NodeHandle& nh, ros::NodeHandle& pnh, const std::vector<std::string>& motor_names)
  : manager_(master, asi, avi, api, nh, pnh, motor_names)
{
  // TODO throw exception or something
  try {
    transmission_loader.reset(new transmission_interface::TransmissionInterfaceLoader(this, &robot_transmissions));
  }
  catch(const std::invalid_argument& ex){
    ROS_ERROR_STREAM("Failed to create transmission interface loader. " << ex.what());
    return;
  }
  catch(const pluginlib::LibraryLoadException& ex){
    ROS_ERROR_STREAM("Failed to create transmission interface loader. " << ex.what());
    return;
  }
  catch(...){
    ROS_ERROR_STREAM("Failed to create transmission interface loader. ");
    return;
  }

  registerInterface(&asi);
  registerInterface(&avi);
  registerInterface(&api);

  std::string urdf_string;
  nh.getParam("robot_description", urdf_string);
  while (urdf_string.empty() && ros::ok())
  {
    ROS_INFO_STREAM_ONCE("Waiting for robot_description");
    nh.getParam("robot_description", urdf_string);
    ros::Duration(0.1).sleep();
  }

  transmission_interface::TransmissionParser parser;
  std::vector<transmission_interface::TransmissionInfo> infos;
  // TODO: throw exception
  if (!parser.parse(urdf_string, infos))
  {
    ROS_ERROR("Error parsing URDF");
    return;
  }

  // build a list of all loaded actuator names
  std::vector<std::string> actuator_names;
  std::vector<std::shared_ptr<KaCanopenMotor> > motors = manager_.motors();
  for (const auto& motor : motors)
  {
    actuator_names.push_back(motor->actuatorName());
  }

  // Load all transmissions that are for the loaded motors
  for (const auto& info : infos)
  {
    bool found_some = false;
    bool found_all = true;
    for (const auto& actuator : info.actuators_) {
      if(std::find(actuator_names.begin(), actuator_names.end(), actuator.name_) != actuator_names.end())
        found_some = true;
      else
        found_all = false;
    }
    if (found_all)
    {
      if (!transmission_loader->load(info))
      {
        ROS_ERROR_STREAM("Error loading transmission: " << info.name_);
        return;
      }
      else
        ROS_INFO_STREAM("Loaded transmission: " << info.name_);
    }
    else if (found_some)
      ROS_ERROR_STREAM("Do not support transmissions that contain only some kacanopen actuators: " << info.name_);
  }

}

bool KaCanopenHardware::init()
{
  return manager_.init();
}

void KaCanopenHardware::updateDiagnostics()
{
  manager_.updateDiagnostics();
}

void KaCanopenHardware::read()
{
  manager_.read();
  if(robot_transmissions.get<transmission_interface::ActuatorToJointStateInterface>())
    robot_transmissions.get<transmission_interface::ActuatorToJointStateInterface>()->propagate();
}

void KaCanopenHardware::write()
{
  if(robot_transmissions.get<transmission_interface::JointToActuatorVelocityInterface>())
    robot_transmissions.get<transmission_interface::JointToActuatorVelocityInterface>()->propagate();
  if(robot_transmissions.get<transmission_interface::JointToActuatorPositionInterface>())
    robot_transmissions.get<transmission_interface::JointToActuatorPositionInterface>()->propagate();
  manager_.write();
}

}
