#ifndef REMOTE_CONTROLLER_DEVICE_INFO_HPP
#define REMOTE_CONTROLLER_DEVICE_INFO_HPP

#include <string>

namespace remote_controller
{

class DeviceInfoReader
{
public:
  // Read SN from device info file
  // Returns empty string if file not found or SN not present
  static std::string readSerialNumber(
    const std::string & file_path = "/workspace/.info/device_info.json");
};

} // namespace remote_controller

#endif // REMOTE_CONTROLLER_DEVICE_INFO_HPP
