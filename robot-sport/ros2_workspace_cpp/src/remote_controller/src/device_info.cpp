#include "remote_controller/device_info.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace remote_controller
{

std::string DeviceInfoReader::readSerialNumber(const std::string & file_path)
{
  try {
    if (!std::filesystem::exists(file_path)) {
      return "";
    }

    std::ifstream file(file_path);
    if (!file.is_open()) {
      return "";
    }

    nlohmann::json j;
    file >> j;

    if (j.contains("SN") && j["SN"].is_string()) {
      return j["SN"].get<std::string>();
    }

    return "";
  } catch (const std::exception &) {
    return "";
  }
}

} // namespace remote_controller
