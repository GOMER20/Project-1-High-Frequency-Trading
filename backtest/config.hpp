#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// One settings file for everything: the market, the exchange's rules, and the
// strategy. An INI file:
//
//   # a comment
//   [section]
//   name = value
//
// Every setting has a default, so the file need only list what differs. A
// name the program does not know is an error, not something quietly ignored:
// a typo in a risk limit should stop the run, not run without the limit.

namespace bt {

class Settings {
 public:
  // Reads a file. Returns nothing, and puts the reason in `error`, if it
  // cannot be read or does not parse.
  [[nodiscard]] static std::optional<Settings> load(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
      error = "cannot read " + path;
      return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();
    return parse(text.str(), error);
  }

  [[nodiscard]] static std::optional<Settings> parse(const std::string& text, std::string& error) {
    Settings settings;
    std::istringstream lines(text);
    std::string line;
    std::string section;
    int number = 0;
    while (std::getline(lines, line)) {
      ++number;
      const std::size_t comment = line.find_first_of("#;");
      if (comment != std::string::npos) {
        line.resize(comment);
      }
      line = trim(line);
      if (line.empty()) {
        continue;
      }
      if (line.front() == '[') {
        if (line.back() != ']') {
          error = "line " + std::to_string(number) + ": a section name must end with ]";
          return std::nullopt;
        }
        section = trim(line.substr(1, line.size() - 2));
        continue;
      }
      const std::size_t equals = line.find('=');
      if (equals == std::string::npos) {
        error = "line " + std::to_string(number) + ": expected name = value";
        return std::nullopt;
      }
      const std::string name = trim(line.substr(0, equals));
      if (name.empty()) {
        error = "line " + std::to_string(number) + ": a setting needs a name";
        return std::nullopt;
      }
      settings.values_[section.empty() ? name : section + "." + name] = trim(line.substr(equals + 1));
    }
    return settings;
  }

  // Overrides one setting, written "section.name=value": for the command line
  // and for parameter sweeps.
  void set(const std::string& key, const std::string& value) { values_[key] = value; }

  [[nodiscard]] double number(const std::string& key, double fallback) {
    used_.insert(key);
    const auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str() || *end != '\0') {
      bad_.push_back(key + " = " + it->second + " is not a number");
      return fallback;
    }
    return value;
  }

  [[nodiscard]] std::int64_t integer(const std::string& key, std::int64_t fallback) {
    const double value = number(key, static_cast<double>(fallback));
    if (value != static_cast<double>(static_cast<std::int64_t>(value))) {
      bad_.push_back(key + " must be a whole number");
      return fallback;
    }
    return static_cast<std::int64_t>(value);
  }

  [[nodiscard]] bool flag(const std::string& key, bool fallback) {
    used_.insert(key);
    const auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    if (it->second == "true" || it->second == "yes" || it->second == "on" || it->second == "1") {
      return true;
    }
    if (it->second == "false" || it->second == "no" || it->second == "off" || it->second == "0") {
      return false;
    }
    bad_.push_back(key + " = " + it->second + " is not true or false");
    return fallback;
  }

  [[nodiscard]] std::string text(const std::string& key, const std::string& fallback) {
    used_.insert(key);
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
  }

  // A comma-separated list.
  [[nodiscard]] std::vector<std::string> list(const std::string& key, const std::vector<std::string>& fallback) {
    used_.insert(key);
    const auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    std::vector<std::string> items;
    std::istringstream parts(it->second);
    std::string item;
    while (std::getline(parts, item, ',')) {
      item = trim(item);
      if (!item.empty()) {
        items.push_back(item);
      }
    }
    return items;
  }

  // Notes a problem found by whoever reads the settings: a value out of range.
  void complain(const std::string& problem) { bad_.push_back(problem); }

  // After everything has been read: what was wrong, if anything. A setting in
  // the file that nothing asked for is reported as unknown.
  [[nodiscard]] std::vector<std::string> problems() const {
    std::vector<std::string> found = bad_;
    for (const auto& [key, value] : values_) {
      if (!used_.contains(key)) {
        found.push_back("unknown setting " + key);
      }
    }
    return found;
  }

 private:
  [[nodiscard]] static std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
      return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
  }

  std::map<std::string, std::string> values_;
  std::set<std::string> used_;
  std::vector<std::string> bad_;
};

}  // namespace bt
