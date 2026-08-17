#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace robot_switch_server {

inline std::string TrimCopy(const std::string& s) {
  constexpr char kWhitespace[] = " \t\n\r\f\v";
  const auto start = s.find_first_not_of(kWhitespace);
  if (start == std::string::npos) {
    return "";
  }
  const auto end = s.find_last_not_of(kWhitespace);
  return s.substr(start, end - start + 1);
}

inline std::string ToLowerCopy(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return result;
}

}  // namespace robot_switch_server
