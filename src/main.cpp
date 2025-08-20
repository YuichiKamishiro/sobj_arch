#include "Agents.hpp"
#include "CommandQueue.hpp"
#include "JsonParser.hpp"
#include "NetworkUtils.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <so_5/all.hpp>
#include <thread>
#include <unordered_map>

std::atomic<bool> running{true};

void signal_handler(int signal) {
  std::cout << "Received signal " << signal << ", shutting down..."
            << std::endl;
  running.store(false);
  std::thread force_exit([] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::exit(1);
  });
  force_exit.detach();
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv << " <config.json> [--test-mode]"
              << std::endl;
    return 1;
  }

  std::string path = argv[1];
  bool test_mode = (argc > 2 && std::string(argv[2]) == "--test-mode");

  auto config_opt = ConfigParser::parse(path, test_mode);
  if (!config_opt) {
    return 1;
  }

  Config config = config_opt.value();
  std::cout << "Config parsed successfully!" << std::endl;

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  int queue_size = 1000;
  if (config.cmd.agent_settings) {
    queue_size = config.cmd.agent_settings->queue_size;
  }

  CommandQueue command_queue(queue_size);
  CommandQueue msc_queue(queue_size);
  std::thread epoll_thr(
      [&]() { epoll_thread(config, command_queue, msc_queue, running); });

  try {
    so_5::launch([&](so_5::environment_t &env) {
      using namespace so_5::disp::adv_thread_pool;
      so_5::mbox_t dispatcher_mbox;
      CommandDispatcherAgent *dispatcher;

      env.introduce_coop(make_dispatcher(env, 3).binder(
                             bind_params_t{}.fifo(fifo_t::individual)),
                         [&](so_5::coop_t &coop) {
                           dispatcher = coop.make_agent<CommandDispatcherAgent>(
                               std::cref(config));
                         });

      env.introduce_coop(
          so_5::disp::active_obj::make_dispatcher(env).binder(),
          [&](so_5::coop_t &coop) {
            auto broadcaster_mbox =
                coop.make_agent<EventBroadcasterAgent>(std::cref(config))
                    ->so_direct_mbox();

            auto final_reponser = coop.make_agent<FinalResponseAgent>();
            auto final_reponser_mbox = final_reponser->so_direct_mbox();

            auto dispatcher_mbox = dispatcher->so_direct_mbox();

            std::unordered_map<std::string, so_5::mbox_t> msc_mboxes;
            for (const auto &msc_config : config.msc_agents) {
              auto msc_agent = coop.make_agent<MscAgent>(
                  std::cref(msc_config), broadcaster_mbox, dispatcher_mbox,
                  std::ref(msc_queue));
              msc_mboxes[msc_config.id] = msc_agent->so_direct_mbox();
            }

            auto ingress_agent = coop.make_agent<CommandIngressAgent>(
                std::ref(command_queue), std::cref(config), test_mode,
                dispatcher_mbox);
            auto ingress_mbox = ingress_agent->so_direct_mbox();

            dispatcher->set_links(std::move(msc_mboxes), final_reponser_mbox);
          });

      while (running.load()) {
        // std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      env.stop();
    });
  } catch (const std::exception &e) {
    std::cerr << "SObjectizer error: " << e.what() << std::endl;
    running.store(false);
  }

  if (epoll_thr.joinable()) {
    epoll_thr.join();
  }

  std::cout << "Application shutdown complete." << std::endl;
  return 0;
}
