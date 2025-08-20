#ifndef AGENTS_H
#define AGENTS_H

#include "CommandQueue.hpp"
#include "JsonParser.hpp"
#include "Messages.hpp"
#include "NetworkUtils.hpp"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <so_5/all.hpp>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#define DEBUG

using json = nlohmann::json;

class FinalResponseAgent final : public so_5::agent_t {
private:
  bool test_mode_;

public:
  FinalResponseAgent(so_5::agent_context_t ctx) : so_5::agent_t(ctx) {}
  void so_define_agent() override {
    so_subscribe_self().event(
        [this](so_5::mhood_t<FinalResponse> msg) { send_final_response(msg); });
  }

  void so_evt_start() { std::cout << "[FinalResponseAgent] started\n"; }

  void so_evt_finish() {}

private:
  // Отправка финального ответа клиенту
  void send_final_response(so_5::mhood_t<FinalResponse> msg) {
    send_udp(msg->destination, msg->response_json);
#ifdef DEBUG
    std::cout << "[Final Responser] Final response sent" << std::endl;
#endif
  }
};

class CommandIngressAgent final : public so_5::agent_t {
private:
  // Очередь из сообщений медленных
  CommandQueue &queue_;
  // Уже распаршенный конфиг json
  const Config &config_;
  // Передан ли параметр --test-mode
  bool test_mode_;
  // MailBox диспатчера
  so_5::mbox_t dispatcher_mbox_;
  // Таймер для периодической проверки очереди
  so_5::timer_id_t timer_;
  // Счетчик запросов для уникальных ID
  uint64_t request_counter_ = 0;

public:
  CommandIngressAgent(so_5::agent_context_t ctx, CommandQueue &queue,
                      const Config &config, bool test_mode,
                      so_5::mbox_t dispatcher_mbox)
      : so_5::agent_t(ctx), queue_(queue), config_(config),
        test_mode_(test_mode), dispatcher_mbox_(dispatcher_mbox) {}

  void so_define_agent() override {
    // Подписка на сигнал обработки сообщений + финальный ответ
    so_subscribe_self().event(
        [this](so_5::mhood_t<ProcessQueue>) { process_queue(); });
  }

  void so_evt_start() override {
    // Каждые 10 миллисекунд с паузой 0 смотреть очередь на сообщения
    timer_ = so_5::send_periodic<ProcessQueue>(
        *this, std::chrono::milliseconds(0), std::chrono::milliseconds(10));
#ifdef DEBUG
    std::cout << "[INGRESS] Agent started" << std::endl;
#endif
  }

  void so_evt_finish() override {
    // Освобождаем таймер при завершении
    timer_.release();
  }

private:
  // Код обработки пакетов из очереди
  void process_queue() {
    // Достаем пакет из очереди
    auto pkt_opt = queue_.pop();
    if (!pkt_opt)
      return;

    const Packet &pkt = pkt_opt.value();
    try {
      // Парсим json
      json j =
          json::parse(std::string(pkt.buf.begin(), pkt.buf.begin() + pkt.len));

      // Если нет command кидаем runtime_error
      if (!j.is_object() || !j.contains("command") ||
          !j["command"].is_string()) {
        throw std::runtime_error("Invalid format or missing 'command' field");
      }

      // Отправляем на remove предварительное сообщение
      send_udp(
          parse_address(config_.cmd.remote_address),
          R"({"status":"accepted","message":"Command received for processing"})");

      if (test_mode_) {
        std::cout << "[INGRESS] Test-mode JSON:\n" << j.dump(4) << std::endl;
      }

      // Генерируем простой ID запроса
      std::string request_id = "req_" + std::to_string(++request_counter_);

      // Отправка Валидированной комманды
      so_5::send<ValidatedCommand>(dispatcher_mbox_, std::move(j),
                                   pkt.sender_addr, request_id);

#ifdef DEBUG
      std::cout << "[INGRESS] Command forwarded: " << request_id << std::endl;
#endif

    } catch (const std::exception &e) {
      // Отвечаем клиенту об ошибке валидации
      json error = {{"error", "validation_failed"}, {"message", e.what()}};
      send_udp(pkt.sender_addr, error.dump());
      std::cerr << "[INGRESS] Validation failed: " << e.what() << std::endl;
    }
  }
};

class CommandDispatcherAgent final : public so_5::agent_t {
private:
  // Структура для отслеживания ожидающих запросов
  struct PendingRequest {
    std::vector<std::string> waiting_for; // Агенты от которых ждем ответ
    std::vector<json> responses;          // Полученные ответы
    sockaddr_in original_sender;          // Адрес оригинального отправителя
    std::chrono::steady_clock::time_point start_time; // Время начала обработки
  };

  // Json конфиг
  const Config &config_;
  // MailBoxы MSC агентов
  std::unordered_map<std::string, so_5::mbox_t> msc_mboxes_;
  // MailBox Ingress агента
  so_5::mbox_t ingress_mbox_;
  // Мапа ожидающих запросов по их ID
  std::unordered_map<std::string, PendingRequest> pending_requests_;
  // Таймер для проверки таймаутов
  so_5::timer_id_t check_timer_;

public:
  CommandDispatcherAgent(so_5::agent_context_t ctx, const Config &config)
      : so_5::agent_t(ctx), config_(config) {}

  // Установка правильных MailBox.
  // Почему то при создании глабольного в main а после прокидывании все идет
  // максимально коряво
  void set_links(std::unordered_map<std::string, so_5::mbox_t> msc_mboxes,
                 so_5::mbox_t ingress_mbox) {
    msc_mboxes_ = std::move(msc_mboxes);
    ingress_mbox_ = ingress_mbox;

#ifdef DEBUG
    std::cout << "[DISPATCHER] Linked with " << msc_mboxes_.size()
              << " MSC agents" << std::endl;
#endif
  }

  void so_define_agent() override {
    // Подписка на получении Валидированных комманд, AgentReply, CheckResponse.
    so_subscribe_self()
        .event(
            [this](so_5::mhood_t<ValidatedCommand> cmd) {
              handle_validated_command(cmd);
            },
            so_5::thread_safe)
        .event(
            [this](so_5::mhood_t<AgentReply> reply) {
              handle_agent_reply(reply);
            },
            so_5::thread_safe)
        .event([this](so_5::mhood_t<CheckResponses>) { check_timeouts(); });
  }

  void so_evt_start() override {
    // Каждые 100мс проверяем таймауты запросов
    check_timer_ = so_5::send_periodic<CheckResponses>(
        *this, std::chrono::milliseconds(0), std::chrono::milliseconds(10));
#ifdef DEBUG
    std::cout << "[DISPATCHER] Agent started" << std::endl;
#endif
  }

  void so_evt_finish() override {
    // Освобождаем таймер при завершении
    check_timer_.release();
  }

private:
  // Обработка валидированной команды от Ingress агента
  void handle_validated_command(so_5::mhood_t<ValidatedCommand> msg) {
    std::vector<std::string> targets;
    std::string target = msg->cmd.value("target", "");

    // Определяем целевые агенты для отправки команды
    if (target == "all") {
      // Отправляем всем доступным MSC агентам
      for (auto &[id, _] : msc_mboxes_) {
        targets.push_back(id);
      }
    } else if (!target.empty() && msc_mboxes_.count(target)) {
      // Отправляем конкретному MSC агенту
      targets.push_back(target);
    } else {
      // Целевой агент не найден - отвечаем ошибкой
      std::cerr << "[DISPATCHER] Invalid target: " << target << std::endl;
      so_5::send<FinalResponse>(
          ingress_mbox_,
          R"({"error":"invalid_target","message":"Target not found"})",
          msg->original_sender);
      return;
    }

    if (targets.empty()) {
      std::cerr << "[DISPATCHER] No targets found" << std::endl;
      so_5::send<FinalResponse>(
          ingress_mbox_,
          R"({"error":"no_targets","message":"No valid targets found"})",
          msg->original_sender);
      return;
    }

    // Создаем запись для отслеживания ответов
    PendingRequest &pending = pending_requests_[msg->request_id];
    pending.waiting_for = targets;
    pending.original_sender = msg->original_sender;
    pending.start_time = std::chrono::steady_clock::now();

    // Отправляем команды всем целевым агентам
    for (const auto &target_id : targets) {
      so_5::send<SubCommand>(msc_mboxes_[target_id], msg->cmd, msg->request_id,
                             target_id);
    }

#ifdef DEBUG
    std::cout << "[DISPATCHER] Command dispatched to " << targets.size()
              << " agents: " << msg->request_id << std::endl;
#endif
  }

  // Обработка ответа от MSC агента
  void handle_agent_reply(so_5::mhood_t<AgentReply> reply) {
    auto it = pending_requests_.find(reply->request_id);
    if (it == pending_requests_.end()) {
      return;
    }

    PendingRequest &pending = it->second;

    // Добавляем информацию об агенте к ответу
    json response_with_agent = reply->response;
    response_with_agent["agent_id"] = reply->agent_id;
    response_with_agent["success"] = reply->success;
    pending.responses.push_back(response_with_agent);

    // Убираем агента из списка ожидания
    auto waiting_it = std::find(pending.waiting_for.begin(),
                                pending.waiting_for.end(), reply->agent_id);
    if (waiting_it != pending.waiting_for.end()) {
      pending.waiting_for.erase(waiting_it);
    }

    // Если получили все ответы - отправляем финальный ответ
    if (pending.waiting_for.empty()) {
      send_final_response(reply->request_id);
    }
  }

  // Проверка таймаутов для ожидающих запросов
  void check_timeouts() {
    auto now = std::chrono::steady_clock::now();

    for (auto it = pending_requests_.begin(); it != pending_requests_.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - it->second.start_time)
                         .count();

      if (elapsed >= config_.cmd.response_timeout_ms) {
        // Запрос превысил таймаут
        std::string request_id = it->first;
        PendingRequest pending = it->second;

#ifdef DEBUG
        std::cout << "[DISPATCHER] Timeout: " << request_id << std::endl;
#endif

        // Добавляем ошибки таймаута для не ответивших агентов
        for (const std::string &missing_agent : pending.waiting_for) {
          json timeout_response;
          timeout_response["error"] = "timeout";
          timeout_response["agent_id"] = missing_agent;
          timeout_response["success"] = false;
          pending.responses.push_back(timeout_response);
        }

        it = pending_requests_.erase(it);
        send_final_response_safe(request_id, pending);
      } else {
        ++it;
      }
    }
  }

  // Отправка финального ответа (безопасный вариант)
  void send_final_response(const std::string &request_id) {
    auto it = pending_requests_.find(request_id);
    if (it == pending_requests_.end())
      return;

    // Копируем данные перед удалением записи
    PendingRequest pending = it->second;
    pending_requests_.erase(it);

    send_final_response_safe(request_id, pending);
  }

  // Безопасная отправка финального ответа с уже скопированными данными
  void send_final_response_safe(const std::string &request_id,
                                const PendingRequest &pending) {
    json final_response;
    final_response["status"] = "completed";
    final_response["request_id"] = request_id;
    final_response["responses"] = pending.responses;

    so_5::send<FinalResponse>(ingress_mbox_, final_response.dump(),
                              pending.original_sender);

#ifdef DEBUG
    std::cout << "[DISPATCHER] Final response prepared: " << request_id
              << std::endl;
#endif
  }
};

class MscAgent final : public so_5::agent_t {
private:
  const MscAgentSettings &settings_;
  so_5::mbox_t broadcaster_;
  so_5::mbox_t dispatcher_mbox_;
  CommandQueue &msc_queue_;
  so_5::timer_id_t process_timer_;

public:
  MscAgent(so_5::agent_context_t ctx, const MscAgentSettings &settings,
           so_5::mbox_t broadcaster_mbox, so_5::mbox_t dispatcher_mbox,
           CommandQueue &msc_queue)
      : so_5::agent_t(ctx), settings_(settings), broadcaster_(broadcaster_mbox),
        dispatcher_mbox_(dispatcher_mbox), msc_queue_(msc_queue) {}

  void so_define_agent() override {
    so_subscribe_self()
        .event(&MscAgent::handle_command)
        .event(&MscAgent::process_incoming_packets);
  }

  void so_evt_start() override {
    process_timer_ = so_5::send_periodic<ProcessIncomingPackets>(
        *this, std::chrono::milliseconds(10), std::chrono::milliseconds(10));

#ifdef DEBUG
    std::cout << "[MSC-" << settings_.id << "] Agent started" << std::endl;
#endif
  }

  void so_evt_finish() override { process_timer_.release(); }

private:
  void handle_command(const so_5::mhood_t<SubCommand> &msg) {
    // Отправляем команду во внешнюю систему используя общую функцию send_udp
    sockaddr_in remote = parse_address(settings_.remote_address);
    std::string body = msg->sub_cmd.dump();
    send_udp(remote, body);
#ifdef DEBUG
    std::cout << "[MSC-" << settings_.id << "] Command sent to external system"
              << std::endl;
#endif

    so_5::send<AgentReply>(
        dispatcher_mbox_,
        json{{"result", "success"}, {"message", "Command processed"}},
        msg->request_id, settings_.id, true);
  }

  void process_incoming_packets(const so_5::mhood_t<ProcessIncomingPackets> &) {
    while (true) {
      auto pkt_opt = msc_queue_.pop_for_agent(settings_.id);
      if (!pkt_opt)
        break;

      const Packet &pkt = pkt_opt.value();

      try {
        std::string data_str(pkt.buf.begin(), pkt.buf.begin() + pkt.len);
        json data = json::parse(data_str);

        if (data.contains("request_id")) {
          // Синхронный ответ на команду → dispatcher
          std::string request_id = data["request_id"];

          so_5::send<AgentReply>(dispatcher_mbox_, data, request_id,
                                 settings_.id, true); // Почему то не работает надо разобраться

#ifdef DEBUG
          std::cout << "[MSC-" << settings_.id
                    << "] Sync response forwarded: " << request_id << std::endl;
#endif
        } else {
          so_5::send<Event>(broadcaster_, data); // Работает шикарно)

#ifdef DEBUG
          std::cout << "[MSC-" << settings_.id << "] Async event forwarded: "
                    << data.value("event", "unknown") << std::endl;
#endif
        }

      } catch (const std::exception &e) {
        std::cerr << "[MSC-" << settings_.id << "] Parse error: " << e.what()
                  << std::endl;
      }
    }
  }
};

class EventBroadcasterAgent final : public so_5::agent_t {
private:
  // Конфигурация для получения адреса трансляции
  const Config &config_;

public:
  EventBroadcasterAgent(so_5::agent_context_t ctx, const Config &cfg)
      : so_5::agent_t(ctx), config_(cfg) {}

  void so_define_agent() override {
    // Подписка на события от MSC агентов
    so_subscribe_self().event(
        [this](so_5::mhood_t<Event> ev) { broadcast_event(ev); });
  }

  void so_evt_start() override {
#ifdef DEBUG
    std::cout << "[BROADCASTER] Agent started" << std::endl;
#endif
  }

private:
  // Трансляция события по UDP на удаленный адрес
  void broadcast_event(so_5::mhood_t<Event> ev) {
    send_udp(parse_address(config_.cmd.remote_address), ev->event_data.dump());

#ifdef DEBUG
    std::cout << "[BROADCASTER] Event sent: "
              << ev->event_data.value("event", "unknown") << std::endl;
#endif
  }
};

#endif
