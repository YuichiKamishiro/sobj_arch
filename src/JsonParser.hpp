#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

struct AgentSettings {
  int queue_size = 1000;
  int default_timeout_ms = 2000;

  std::string to_string() const {
    return "queue_size: " + std::to_string(queue_size) +
           ", default_timeout_ms: " + std::to_string(default_timeout_ms);
  }
};

struct CmdSettings {
  std::string local_address;
  std::string remote_address;
  int response_timeout_ms;
  std::optional<AgentSettings> agent_settings;
  std::string to_string() const {
    std::string str = "Cmd: local=" + local_address +
                      ", remote=" + remote_address +
                      ", timeout=" + std::to_string(response_timeout_ms);
    if (agent_settings) {
      str += ", settings={" + agent_settings->to_string() + "}";
    }
    return str;
  }
};

struct MscAgentSettings {
  std::string id;
  std::string local_address;
  std::string remote_address;
  int response_timeout_ms;
  std::optional<AgentSettings> agent_settings;

  std::string to_string() const {
    std::string str = "MscAgent id=" + id + ": local=" + local_address +
                      ", remote=" + remote_address +
                      ", timeout=" + std::to_string(response_timeout_ms);
    if (agent_settings) {
      str += ", settings={" + agent_settings->to_string() + "}";
    }
    return str;
  }
};

struct StreamPortSettings {
  std::string id;
  std::string local_address;
  std::string remote_address;
  std::string format;

  std::string to_string() const {
    return "StreamPort id=" + id + ": local=" + local_address +
           ", remote=" + remote_address + ", format=" + format;
  }
};

struct Config {
  CmdSettings cmd;
  std::vector<MscAgentSettings> msc_agents;
  std::vector<StreamPortSettings> stream_ports;

  void log() const {
    std::cout << "Parsed Config:\n";
    std::cout << cmd.to_string() << "\n";
    for (const auto &msc : msc_agents) {
      std::cout << msc.to_string() << "\n";
    }
    for (const auto &stream : stream_ports) {
      std::cout << stream.to_string() << "\n";
    }
  }
};

class ConfigParser {
public:
  static std::optional<Config> parse(const std::string &path, bool test_mode) {
    std::ifstream file(path);
    if (!file.is_open()) {
      std::cerr << "Error: Cannot open config file: " << path << std::endl;
      exit(1);
    }

    json config_json;
    try {
      config_json = json::parse(file);
    } catch (const json::parse_error &e) {
      std::cerr << "Error: Invalid JSON format: " << e.what() << std::endl;
      exit(1);
    }

    Config config;

    if (!config_json.contains("cmd") || !config_json["cmd"].is_object()) {
      std::cerr << "Error: Missing or invalid 'cmd' section" << std::endl;
      exit(1);
    }
    auto &cmd_json = config_json["cmd"];
    if (!cmd_json.contains("local_address") ||
        !cmd_json["local_address"].is_string() ||
        !cmd_json.contains("remote_address") ||
        !cmd_json["remote_address"].is_string() ||
        !cmd_json.contains("response_timeout_ms") ||
        !cmd_json["response_timeout_ms"].is_number_integer()) {
      std::cerr << "Error: Invalid fields in 'cmd'" << std::endl;
      exit(1);
    }
    config.cmd.local_address = cmd_json["local_address"];
    config.cmd.remote_address = cmd_json["remote_address"];
    config.cmd.response_timeout_ms = cmd_json["response_timeout_ms"];

    if (cmd_json.contains("agent_settings") &&
        cmd_json["agent_settings"].is_object()) {
      auto &settings_json = cmd_json["agent_settings"];
      AgentSettings settings;
      if (settings_json.contains("queue_size") &&
          settings_json["queue_size"].is_number_integer()) {
        settings.queue_size = settings_json["queue_size"];
      }
      if (settings_json.contains("default_timeout_ms") &&
          settings_json["default_timeout_ms"].is_number_integer()) {
        settings.default_timeout_ms = settings_json["default_timeout_ms"];
      }
      config.cmd.agent_settings = settings;
    }

    if (!config_json.contains("msc_agent") ||
        !config_json["msc_agent"].is_array()) {
      std::cerr << "Error: Missing or invalid 'msc_agent' array" << std::endl;
      exit(1);
    }
    for (const auto &item : config_json["msc_agent"]) {
      if (!item.is_object() || !item.contains("id") ||
          !item["id"].is_string() || !item.contains("local_address") ||
          !item["local_address"].is_string() ||
          !item.contains("remote_address") ||
          !item["remote_address"].is_string() ||
          !item.contains("response_timeout_ms") ||
          !item["response_timeout_ms"].is_number_integer()) {
        std::cerr << "Error: Invalid item in 'msc_agent'" << std::endl;
        exit(1);
      }
      MscAgentSettings msc;
      msc.id = item["id"];
      msc.local_address = item["local_address"];
      msc.remote_address = item["remote_address"];
      msc.response_timeout_ms = item["response_timeout_ms"];

      if (item.contains("agent_settings") &&
          item["agent_settings"].is_object()) {
        auto &settings_json = item["agent_settings"];
        AgentSettings settings;
        if (settings_json.contains("queue_size") &&
            settings_json["queue_size"].is_number_integer()) {
          settings.queue_size = settings_json["queue_size"];
        }
        if (settings_json.contains("default_timeout_ms") &&
            settings_json["default_timeout_ms"].is_number_integer()) {
          settings.default_timeout_ms = settings_json["default_timeout_ms"];
        }
        msc.agent_settings = settings;
      }
      config.msc_agents.push_back(msc);
    }

    if (!config_json.contains("stream_ports") ||
        !config_json["stream_ports"].is_array()) {
      std::cerr << "Error: Missing or invalid 'stream_ports' array"
                << std::endl;
      exit(1);
    }
    for (const auto &item : config_json["stream_ports"]) {
      if (!item.is_object() || !item.contains("id") ||
          !item["id"].is_string() || !item.contains("local_address") ||
          !item["local_address"].is_string() ||
          !item.contains("remote_address") ||
          !item["remote_address"].is_string() || !item.contains("format") ||
          !item["format"].is_string()) {
        std::cerr << "Error: Invalid item in 'stream_ports'" << std::endl;
        exit(1);
      }
      StreamPortSettings stream;
      stream.id = item["id"];
      stream.local_address = item["local_address"];
      stream.remote_address = item["remote_address"];
      stream.format = item["format"];
      config.stream_ports.push_back(stream);
    }

    if (test_mode) {
      config.log();
    }

    return config;
  }
};

#endif