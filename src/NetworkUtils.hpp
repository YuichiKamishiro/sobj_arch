#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#include "CommandQueue.hpp"
#include "JsonParser.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#define DEBUG

// Отправка пакета по UDP
void send_udp(const sockaddr_in &addr, const std::string &message) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    std::cerr << "Ошибка: невозможно создать UDP сокет" << std::endl;
    return;
  }
  if (sendto(sock, message.c_str(), message.size(), 0, (struct sockaddr *)&addr,
             sizeof(addr)) < 0) {
    std::cerr << "Ошибка: sendto" << std::endl;
  }
#ifdef DEBUG
  std::cout << "DEBUG: Отправлен UDP пакет: " << message << std::endl;
#endif
  close(sock);
}

// Разбор строки "ip:port" в sockaddr_in
sockaddr_in parse_address(const std::string &addr_str) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  size_t colon = addr_str.find(':');
  if (colon == std::string::npos) {
    std::cerr << "Ошибка: неверный формат адреса" << std::endl;
    return addr;
  }
  std::string ip = addr_str.substr(0, colon);
  int port = std::stoi(addr_str.substr(colon + 1));
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
    std::cerr << "Ошибка: неверный IP" << std::endl;
  }
#ifdef DEBUG
  std::cout << "DEBUG: Распознан адрес: " << ip << ":" << port << std::endl;
#endif
  return addr;
}

// Поток обработки epoll для приема пакетов
void epoll_thread(const Config &config, CommandQueue &command_queue,
                  CommandQueue &msc_queue, std::atomic<bool> &running,
                  std::unordered_map<std::string, so_5::mbox_t> msc_mboxes) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    std::cerr << "Ошибка: epoll_create" << std::endl;
    return;
  }

  std::unordered_map<int, std::string> fd_to_id;
  std::vector<int> sockets;

  auto add_socket = [&](const std::string &addr_str, const std::string &id) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    sockaddr_in local = parse_address(addr_str);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
      std::cerr << "Ошибка: bind для " << addr_str << std::endl;
      close(sock);
      return;
    }
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
    sockets.push_back(sock);
    fd_to_id[sock] = id;
#ifdef DEBUG
    std::cout << "DEBUG: Добавлен сокет для " << id << " (" << addr_str << ")"
              << std::endl;
#endif
  };

  // Добавляем командный порт
  add_socket(config.cmd.local_address, "cmd");
  // Добавляем MSC порты
  for (const auto &msc : config.msc_agents) {
    add_socket(msc.local_address, "msc_" + msc.id);
  }

  struct epoll_event events[10];
  while (running) {
    int nfds = epoll_wait(epoll_fd, events, 10, 100);
    if (nfds < 0)
      continue;
    for (int i = 0; i < nfds; ++i) {
      int fd = events[i].data.fd;
      sockaddr_in sender{};
      socklen_t slen = sizeof(sender);
      char buf[4096];
      ssize_t recv_len =
          recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&sender, &slen);
      if (recv_len > 0) {
        std::string port_id = fd_to_id[fd];
        std::vector<uint8_t> buffer_data(reinterpret_cast<uint8_t *>(buf),
                                         reinterpret_cast<uint8_t *>(buf) +
                                             recv_len);
        Packet pkt{buffer_data, static_cast<size_t>(recv_len), port_id, sender};
        if (port_id.starts_with("msc_")) {
          std::string agent_id = port_id.substr(4);
          auto it = msc_mboxes.find(agent_id);
          if (it != msc_mboxes.end()) {
            so_5::send<Packet>(it->second, pkt);
          } else {
             std::cerr << "ERROR: Mailbox for agent " << agent_id 
                       << " not found. Packet dropped." << std::endl;
          }
#ifdef DEBUG
          std::cout << "DEBUG: MSC пакет из " << port_id << ", размер "
                    << recv_len << std::endl;
#endif
        } else {
          command_queue.push(pkt); // Пакеты команд
#ifdef DEBUG
          std::cout << "DEBUG: CMD пакет, размер " << recv_len << std::endl;
#endif
        }
      } else if (recv_len < 0) {
        std::cerr << "Ошибка: recvfrom" << std::endl;
      }
    }
  }

  for (int sock : sockets)
    close(sock);
  close(epoll_fd);
#ifdef DEBUG
  std::cout << "DEBUG: Поток epoll завершён" << std::endl;
#endif
}

#endif
