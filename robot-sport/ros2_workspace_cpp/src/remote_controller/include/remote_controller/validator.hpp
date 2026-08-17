#ifndef REMOTE_CONTROLLER_VALIDATOR_HPP
#define REMOTE_CONTROLLER_VALIDATOR_HPP

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include "remote_controller/response.hpp"

namespace remote_controller
{

struct ValidationResult
{
  bool is_valid;
  std::vector<ErrorDetail> errors;

  ValidationResult(bool valid = true)
  : is_valid(valid) {}

  void addError(
    ErrorCode code, const std::string & message,
    const std::string & field = "",
    const std::optional<std::string> & suggestion = std::nullopt)
  {
    is_valid = false;
    errors.emplace_back(code, message, field, suggestion);
  }
};

struct VelocityLimits
{
  // Linear velocity limits (m/s)
  double linear_x_min = -3.0;
  double linear_x_max = 3.0;
  double linear_y_min = -3.0;
  double linear_y_max = 3.0;
  double linear_z_min = -3.0;
  double linear_z_max = 3.0;

  // Angular velocity limits (rad/s)
  double angular_x_min = -3.0;
  double angular_x_max = 3.0;
  double angular_y_min = -3.0;
  double angular_y_max = 3.0;
  double angular_z_min = -3.0;
  double angular_z_max = 3.0;
};

class MessageValidator
{
public:
  explicit MessageValidator(const VelocityLimits & limits = VelocityLimits{})
  : limits_(limits) {}

  // Main validation function
  ValidationResult validateVelocityCommand(const nlohmann::json & json);

  // Individual validation functions
  ValidationResult validateJsonStructure(const nlohmann::json & json);
  ValidationResult validateRequiredFields(const nlohmann::json & json);
  ValidationResult validateFieldTypes(const nlohmann::json & json);
  ValidationResult validateFieldValues(const nlohmann::json & json);

  // Helper functions
  bool hasRequiredField(const nlohmann::json & json, const std::string & field);
  bool isValidNumber(const nlohmann::json & json, const std::string & field);
  bool isInRange(double value, double min, double max);

  // Velocity validation helpers
  void validateLinearVelocity(
    const nlohmann::json & json,
    const std::string & field,
    double min_val, double max_val,
    ValidationResult & result);
  void validateAngularVelocity(
    const nlohmann::json & json,
    const std::string & field,
    double min_val, double max_val,
    ValidationResult & result);

  // Configuration
  void setVelocityLimits(const VelocityLimits & limits) {limits_ = limits;}
  const VelocityLimits & getVelocityLimits() const {return limits_;}

private:
  VelocityLimits limits_;

  // Required fields for backward compatibility - only linear_x and angular_z are mandatory
  const std::vector<std::string> required_fields_ = {"linear_x", "angular_z"};

  // All supported velocity fields
  const std::vector<std::string> all_velocity_fields_ = {
    "linear_x", "linear_y", "linear_z",
    "angular_x", "angular_y", "angular_z"
  };
};

} // namespace remote_controller

#endif // REMOTE_CONTROLLER_VALIDATOR_HPP
