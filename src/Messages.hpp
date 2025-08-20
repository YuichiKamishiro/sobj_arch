#ifndef MESSAGES_H
#define MESSAGES_H

#include <nlohmann/json.hpp>
#include <string>
#include <netinet/in.h>
#include <so_5/all.hpp>

using json = nlohmann::json;

struct ValidatedCommand final {
    json cmd;
    sockaddr_in original_sender;
    std::string request_id;
    ValidatedCommand(json c, sockaddr_in s, std::string rid)
        : cmd(std::move(c)), original_sender(s), request_id(std::move(rid)) {}
};

struct SubCommand final {
    json sub_cmd;
    std::string request_id;
    std::string target_agent_id;
    SubCommand(json c, std::string rid, std::string aid)
        : sub_cmd(std::move(c)), request_id(std::move(rid)), target_agent_id(std::move(aid)) {}
};

struct AgentReply final {
    json response;
    std::string request_id;
    std::string agent_id;
    bool success;
    AgentReply(json r, std::string rid, std::string aid, bool s = true)
        : response(std::move(r)), request_id(std::move(rid)), agent_id(std::move(aid)), success(s) {}
};

struct FinalResponse final {
    std::string response_json;
    sockaddr_in destination;
    FinalResponse(std::string r, sockaddr_in d)
        : response_json(std::move(r)), destination(d) {}
};

struct Event final {
    json event_data;
    explicit Event(json e) : event_data(std::move(e)) {}
};

struct IncomingMscPacket final {
    std::string agent_id;
    json packet_data;
    IncomingMscPacket(std::string aid, json data)
        : agent_id(std::move(aid)), packet_data(std::move(data)) {}
};

// Служебные сигналы для агентов
struct ProcessQueue final : public so_5::signal_t {};
struct CheckResponses final : public so_5::signal_t {};
struct ReadResponses final : public so_5::signal_t {};
struct ProcessIncomingPackets final : public so_5::signal_t {};

#endif
