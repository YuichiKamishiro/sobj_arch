#ifndef COMMAND_QUEUE_H
#define COMMAND_QUEUE_H

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>
#include <netinet/in.h>
#include <iostream>

struct Packet {
    std::vector<uint8_t> buf;
    size_t len;
    std::string port_id;        // Идентификатор источника пакета ("cmd" или "msc_N")
    sockaddr_in sender_addr;    // Адрес отправителя
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();
};

struct PacketComparator {
    bool operator()(const Packet& a, const Packet& b) {
        return a.timestamp > b.timestamp;
    }
};

struct CommandQueue {
    std::priority_queue<Packet, std::vector<Packet>, PacketComparator> queue;
    std::mutex mtx;
    std::condition_variable cv;
    size_t max_size;

    CommandQueue(size_t max) : max_size(max) {}

    void push(const Packet& pkt) {
        std::lock_guard lock(mtx);
        if (queue.size() >= max_size) {
            queue.pop();
            std::cerr << "WARN: Очередь переполнена, удален старый пакет" << std::endl;
        }
        queue.push(pkt);
        cv.notify_one();
    }

    std::optional<Packet> pop() {
        std::unique_lock lock(mtx);
        if (cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return !queue.empty(); })) {
            Packet pkt = queue.top();
            queue.pop();
            return pkt;
        }
        return std::nullopt;
    }
};

#endif
