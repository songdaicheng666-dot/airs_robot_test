#ifndef REMOTE_CONTROLLER_RESPONSE_HPP
#define REMOTE_CONTROLLER_RESPONSE_HPP

#include <string>
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>

namespace remote_controller
{

enum class ResponseStatus
{
  SUCCESS = 200,
  BAD_REQUEST = 400,
  VALIDATION_ERROR = 422,
  INTERNAL_ERROR = 500,
  SERVICE_UNAVAILABLE = 503
};

enum class ErrorCode
{
  // Input validation errors
  MISSING_REQUIRED_FIELD = 1001,
  INVALID_DATA_TYPE = 1002,
  VALUE_OUT_OF_RANGE = 1003,
  INVALID_JSON_FORMAT = 1004,

  // Processing errors
  ROS_PUBLISH_FAILED = 2001,
  CONNECTION_ERROR = 2002,

  // System errors
  INTERNAL_PROCESSING_ERROR = 5001,
  SERVICE_OVERLOADED = 5002
};

struct ResponseMetadata
{
  std::string timestamp;
  std::string request_id;
  std::string version = "1.0";
  double processing_time_ms;

  ResponseMetadata()
  {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    timestamp = ss.str();
  }
};

struct ErrorDetail
{
  ErrorCode code;
  std::string message;
  std::string field;
  std::optional<std::string> suggestion;

  ErrorDetail(
    ErrorCode c, const std::string & msg, const std::string & f = "",
    const std::optional<std::string> & hint = std::nullopt)
  : code(c), message(msg), field(f), suggestion(hint) {}
};

struct VelocityData
{
  double linear_x;
  double linear_y;
  double linear_z;
  double angular_x;
  double angular_y;
  double angular_z;

  std::string topic;
  std::string hub_id;
};

class ResponseBuilder
{
public:
  ResponseBuilder & setRequestId(const std::string & id);
  ResponseBuilder & setProcessingTime(double ms);

  // Success responses
  nlohmann::json buildSuccess(const VelocityData & data);
  nlohmann::json buildSuccess(const std::string & message = "Command processed successfully");

  // Error responses
  nlohmann::json buildError(ResponseStatus status, const ErrorDetail & error);
  nlohmann::json buildError(
    ResponseStatus status, ErrorCode code,
    const std::string & message, const std::string & field = "");

  // Validation error helpers
  nlohmann::json buildValidationError(
    const std::string & field, const std::string & message,
    const std::optional<std::string> & suggestion = std::nullopt);
  nlohmann::json buildMissingFieldError(const std::string & field);
  nlohmann::json buildInvalidTypeError(
    const std::string & field,
    const std::string & expected_type);
  nlohmann::json buildOutOfRangeError(const std::string & field, double min, double max);

private:
  ResponseMetadata metadata_;

  nlohmann::json buildBaseResponse(int code, const std::string & msg);
};

// Utility functions for common error messages
std::string formatFieldError(const std::string & field, const std::string & issue);
std::string formatRangeError(const std::string & field, double value, double min, double max);

} // namespace remote_controller

#endif // REMOTE_CONTROLLER_RESPONSE_HPP
