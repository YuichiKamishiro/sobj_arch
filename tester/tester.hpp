#ifndef STRESS_TESTER_HPP
#define STRESS_TESTER_HPP

#include "../src/JsonParser.hpp"
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>

struct TestResult {
    int sent_count = 0;
    int received_count = 0;
    std::vector<double> response_times;
    std::vector<std::string> errors;
    
    double success_rate() const {
        return sent_count > 0 ? (double)received_count / sent_count * 100.0 : 0.0;
    }
};

struct TestConfig {
    std::string target_ip = "127.0.0.1";
    int rate_per_second = 10;
    int duration_seconds = 5;
    std::string test_mode = "cmd"; // "cmd", "msc", "stream", "all"
    std::string specific_msc_id = ""; // для тестирования конкретного MSC агента
    int socket_timeout_ms = 1000;
};

class StressTester {
private:
    Config config_;
    TestConfig test_config_;
    TestResult result_;
    std::atomic<bool> running_{true};
    std::mutex result_mutex_;
    
    int send_sock_ = -1;
    int recv_sock_ = -1;
    
    // Методы
    void setup_sockets();
    void cleanup_sockets();
    void send_command_to_port(int port, const std::string& target_id, int request_id);
    void receive_responses();
    void print_statistics() const;
    
public:
    StressTester(const Config& config, const TestConfig& test_config);
    ~StressTester();
    
    void run_test();
    const TestResult& get_result() const { return result_; }
};

#endif
