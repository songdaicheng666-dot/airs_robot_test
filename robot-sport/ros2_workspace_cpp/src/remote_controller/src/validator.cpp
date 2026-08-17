#include "remote_controller/validator.hpp"
#include <cmath>

namespace remote_controller
{

ValidationResult MessageValidator::validateVelocityCommand(const nlohmann::json & json)
{
  ValidationResult result;

  // Check JSON structure
  auto structure_result = validateJsonStructure(json);
  if (!structure_result.is_valid) {
    result.errors.insert(
      result.errors.end(),
      structure_result.errors.begin(),
      structure_result.errors.end());
    result.is_valid = false;
    return result;     // Early return on structural errors
  }

  // Check required fields
  auto fields_result = validateRequiredFields(json);
  if (!fields_result.is_valid) {
    result.errors.insert(
      result.errors.end(),
      fields_result.errors.begin(),
      fields_result.errors.end());
    result.is_valid = false;
  }

  // Check field types
  auto types_result = validateFieldTypes(json);
  if (!types_result.is_valid) {
    result.errors.insert(
      result.errors.end(),
      types_result.errors.begin(),
      types_result.errors.end());
    result.is_valid = false;
  }

  // Check field values (only if types are valid)
  if (types_result.is_valid) {
    auto values_result = validateFieldValues(json);
    if (!values_result.is_valid) {
      result.errors.insert(
        result.errors.end(),
        values_result.errors.begin(),
        values_result.errors.end());
      result.is_valid = false;
    }
  }

  return result;
}

ValidationResult MessageValidator::validateJsonStructure(const nlohmann::json & json)
{
  ValidationResult result;

  if (json.is_null()) {
    result.addError(
      ErrorCode::INVALID_JSON_FORMAT,
      "Request body is empty or null",
      "",
      "Please provide a valid JSON object");
    return result;
  }

  if (!json.is_object()) {
    result.addError(
      ErrorCode::INVALID_JSON_FORMAT,
      "Request body must be a JSON object",
      "",
      "Wrap your data in curly braces {}");
    return result;
  }

  return result;
}

ValidationResult MessageValidator::validateRequiredFields(const nlohmann::json & json)
{
  ValidationResult result;

  for (const auto & field : required_fields_) {
    if (!hasRequiredField(json, field)) {
      std::string suggestion = "Include \"" + field + "\": <number> in your JSON request";
      result.addError(
        ErrorCode::MISSING_REQUIRED_FIELD,
        "Required field '" + field + "' is missing",
        field,
        suggestion);
    }
  }

  return result;
}

ValidationResult MessageValidator::validateFieldTypes(const nlohmann::json & json)
{
  ValidationResult result;

  // Check all velocity fields that are present in the JSON
  for (const auto & field : all_velocity_fields_) {
    if (json.contains(field) && !isValidNumber(json, field)) {
      std::string suggestion = "Provide a numeric value like \"" + field + "\": 0.5";
      result.addError(
        ErrorCode::INVALID_DATA_TYPE,
        "Field '" + field + "' must be a number",
        field,
        suggestion);
    }
  }

  return result;
}

ValidationResult MessageValidator::validateFieldValues(const nlohmann::json & json)
{
  ValidationResult result;

  // Validate linear velocities
  validateLinearVelocity(json, "linear_x", limits_.linear_x_min, limits_.linear_x_max, result);
  validateLinearVelocity(json, "linear_y", limits_.linear_y_min, limits_.linear_y_max, result);
  validateLinearVelocity(json, "linear_z", limits_.linear_z_min, limits_.linear_z_max, result);

  // Validate angular velocities
  validateAngularVelocity(json, "angular_x", limits_.angular_x_min, limits_.angular_x_max, result);
  validateAngularVelocity(json, "angular_y", limits_.angular_y_min, limits_.angular_y_max, result);
  validateAngularVelocity(json, "angular_z", limits_.angular_z_min, limits_.angular_z_max, result);

  return result;
}

bool MessageValidator::hasRequiredField(const nlohmann::json & json, const std::string & field)
{
  return json.contains(field);
}

bool MessageValidator::isValidNumber(const nlohmann::json & json, const std::string & field)
{
  if (!json.contains(field)) {
    return false;
  }

  const auto & value = json[field];
  return value.is_number_float() || value.is_number_integer();
}

bool MessageValidator::isInRange(double value, double min, double max)
{
  return value >= min && value <= max;
}

void MessageValidator::validateLinearVelocity(
  const nlohmann::json & json,
  const std::string & field,
  double min_val, double max_val,
  ValidationResult & result)
{
  if (json.contains(field) && isValidNumber(json, field)) {
    double value = json[field];
    if (!isInRange(value, min_val, max_val)) {
      std::string suggestion = "Valid range: [" +
        std::to_string(min_val) + ", " +
        std::to_string(max_val) + "]";
      result.addError(
        ErrorCode::VALUE_OUT_OF_RANGE,
        field + " value " + std::to_string(value) + " is outside valid range",
        field,
        suggestion);
    }

    if (std::isnan(value) || std::isinf(value)) {
      result.addError(
        ErrorCode::INVALID_DATA_TYPE,
        field + " cannot be NaN or infinite",
        field,
        "Provide a finite numeric value");
    }
  }
}

void MessageValidator::validateAngularVelocity(
  const nlohmann::json & json,
  const std::string & field,
  double min_val, double max_val,
  ValidationResult & result)
{
  if (json.contains(field) && isValidNumber(json, field)) {
    double value = json[field];
    if (!isInRange(value, min_val, max_val)) {
      std::string suggestion = "Valid range: [" +
        std::to_string(min_val) + ", " +
        std::to_string(max_val) + "]";
      result.addError(
        ErrorCode::VALUE_OUT_OF_RANGE,
        field + " value " + std::to_string(value) + " is outside valid range",
        field,
        suggestion);
    }

    if (std::isnan(value) || std::isinf(value)) {
      result.addError(
        ErrorCode::INVALID_DATA_TYPE,
        field + " cannot be NaN or infinite",
        field,
        "Provide a finite numeric value");
    }
  }
}

} // namespace remote_controller
