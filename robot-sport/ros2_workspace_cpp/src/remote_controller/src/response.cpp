#include "remote_controller/response.hpp"
#include <iomanip>
#include <sstream>
#include <random>

namespace remote_controller
{

ResponseBuilder & ResponseBuilder::setRequestId(const std::string & id)
{
  metadata_.request_id = id;
  return *this;
}

ResponseBuilder & ResponseBuilder::setProcessingTime(double ms)
{
  metadata_.processing_time_ms = ms;
  return *this;
}

nlohmann::json ResponseBuilder::buildSuccess(const VelocityData & data)
{
  auto response = buildBaseResponse(0, "success");

  response["data"] = {
    {"velocity", {
        {"linear_x", data.linear_x},
        {"linear_y", data.linear_y},
        {"linear_z", data.linear_z},
        {"angular_x", data.angular_x},
        {"angular_y", data.angular_y},
        {"angular_z", data.angular_z}
      }},
    {"target", {
        {"topic", data.topic},
        {"hub_id", data.hub_id}
      }}
  };

  return response;
}

nlohmann::json ResponseBuilder::buildSuccess(const std::string & message)
{
  auto response = buildBaseResponse(0, "success");
  response["data"] = {{"message", message}};
  return response;
}

// status 暂未参与构造(响应 code 取自 error.code);保留在签名里供调用方语义对齐与将来扩展,
// 用 [[maybe_unused]] 标记有意不使用,消除 -Wunused-parameter。
nlohmann::json ResponseBuilder::buildError([[maybe_unused]] ResponseStatus status, const ErrorDetail & error)
{
  auto response = buildBaseResponse(static_cast<int>(error.code), error.message);

  nlohmann::json error_details = {
    {"code", static_cast<int>(error.code)},
    {"message", error.message}
  };

  if (!error.field.empty()) {
    error_details["field"] = error.field;
  }

  if (error.suggestion.has_value()) {
    error_details["suggestion"] = error.suggestion.value();
  }

  response["data"] = {{"error_details", error_details}};

  return response;
}

nlohmann::json ResponseBuilder::buildError(
  ResponseStatus status, ErrorCode code,
  const std::string & message, const std::string & field)
{
  ErrorDetail error(code, message, field);
  return buildError(status, error);
}

nlohmann::json ResponseBuilder::buildValidationError(
  const std::string & field,
  const std::string & message,
  const std::optional<std::string> & suggestion)
{
  ErrorDetail error(ErrorCode::INVALID_DATA_TYPE, message, field, suggestion);
  return buildError(ResponseStatus::VALIDATION_ERROR, error);
}

nlohmann::json ResponseBuilder::buildMissingFieldError(const std::string & field)
{
  std::string message = "Required field '" + field + "' is missing";
  std::string suggestion = "Please include '" + field + "' in your request";

  ErrorDetail error(ErrorCode::MISSING_REQUIRED_FIELD, message, field, suggestion);
  return buildError(ResponseStatus::BAD_REQUEST, error);
}

nlohmann::json ResponseBuilder::buildInvalidTypeError(
  const std::string & field,
  const std::string & expected_type)
{
  std::string message = "Field '" + field + "' must be of type " + expected_type;
  ErrorDetail error(ErrorCode::INVALID_DATA_TYPE, message, field);
  return buildError(ResponseStatus::VALIDATION_ERROR, error);
}

nlohmann::json ResponseBuilder::buildOutOfRangeError(
  const std::string & field, double min,
  double max)
{
  std::string message = "Field '" + field + "' must be between " +
    std::to_string(min) + " and " + std::to_string(max);
  std::string suggestion = "Please provide a value within the valid range";

  ErrorDetail error(ErrorCode::VALUE_OUT_OF_RANGE, message, field, suggestion);
  return buildError(ResponseStatus::VALIDATION_ERROR, error);
}

nlohmann::json ResponseBuilder::buildBaseResponse(int code, const std::string & msg)
{
  // Generate request ID if not set
  if (metadata_.request_id.empty()) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    metadata_.request_id = "req_" + std::to_string(dis(gen));
  }

  nlohmann::json response = {
    {"code", code},
    {"msg", msg},
    {"requestId", metadata_.request_id}
  };

  return response;
}

// Utility functions
std::string formatFieldError(const std::string & field, const std::string & issue)
{
  return "Field '" + field + "': " + issue;
}

std::string formatRangeError(const std::string & field, double value, double min, double max)
{
  return "Field '" + field + "' value " + std::to_string(value) +
         " is outside valid range [" + std::to_string(min) + ", " + std::to_string(max) + "]";
}

} // namespace remote_controller
