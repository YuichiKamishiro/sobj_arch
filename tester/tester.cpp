#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <iomanip>

struct TestResult {
    std::atomic<int> sent_count{0};
    std::atomic<int> received_count{0};
    std::vector<double> response_times;
    std::vector<std::string> errors;
    std::mutex result_mutex;
    
    double success_rate() const {
        int sent = sent_count.load();
        int received = received_count.load();
        return sent > 0 ? (double)received / sent * 100.0 : 0.0;
    }
};

class SimpleTester {
private:
    std::string target_ip_;
    int target_port_;
    int response_port_;
    int rate_per_second_;
    int duration_seconds_;
    
    TestResult result_;
    std::atomic<bool> running_{true};
    
    int send_sock_ = -1;
    int recv_sock_ = -1;
    
public:
    SimpleTester(const std::string& target_ip = "127.0.0.1", 
                 int target_port = 11000, 
                 int response_port = 11001,
                 int rate = 10, 
                 int duration = 5)
        : target_ip_(target_ip), target_port_(target_port), response_port_(response_port),
          rate_per_second_(rate), duration_seconds_(duration) {}
    
    ~SimpleTester() {
        cleanup();
    }
    
    bool setup() {
        // Создаем сокет для отправки
        send_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (send_sock_ < 0) {
            std::cerr << "❌ Failed to create send socket" << std::endl;
            return false;
        }
        
        // Создаем сокет для получения ответов
        recv_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (recv_sock_ < 0) {
            std::cerr << "❌ Failed to create receive socket" << std::endl;
            return false;
        }
        
        // Настраиваем таймаут для receive socket
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(recv_sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
        // Биндим receive socket
        sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = htons(response_port_);
        
        if (bind(recv_sock_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            std::cerr << "❌ Failed to bind receive socket to port " << response_port_ << std::endl;
            return false;
        }
        
        std::cout << "✅ Sockets created successfully" << std::endl;
        std::cout << "📡 Will send to: " << target_ip_ << ":" << target_port_ << std::endl;
        std::cout << "📥 Will listen on: " << response_port_ << std::endl;
        
        return true;
    }
    
    void cleanup() {
        if (send_sock_ >= 0) {
            close(send_sock_);
            send_sock_ = -1;
        }
        if (recv_sock_ >= 0) {
            close(recv_sock_);
            recv_sock_ = -1;
        }
    }
    
    void send_command(int request_id) {
        // Создаем простое JSON сообщение
        std::string message = "{"
            "\"command\":\"stress_test\","
            "\"target\":\"1\","
            "\"request_id\":\"stress_" + std::to_string(request_id) + "\","
            "\"data\":\"load_test_data_" + std::to_string(request_id) + "\","
            "\"timestamp\":" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()) +
            "}";
        
        sockaddr_in target_addr{};
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(target_port_);
        inet_pton(AF_INET, target_ip_.c_str(), &target_addr.sin_addr);
        
        if (sendto(send_sock_, message.c_str(), message.size(), 0, 
                   (struct sockaddr*)&target_addr, sizeof(target_addr)) >= 0) {
            result_.sent_count++;
        } else {
            std::lock_guard<std::mutex> lock(result_.result_mutex);
            result_.errors.push_back("Failed to send message " + std::to_string(request_id));
        }
    }
    
    void receive_responses() {
        char buffer[4096];
        sockaddr_in sender_addr{};
        socklen_t addr_len = sizeof(sender_addr);
        
        while (running_) {
            ssize_t received = recvfrom(recv_sock_, buffer, sizeof(buffer), 0,
                                       (struct sockaddr*)&sender_addr, &addr_len);
            
            if (received > 0) {
                auto receive_time = std::chrono::steady_clock::now();
                std::string response(buffer, received);
                
                result_.received_count++;
                
                // Пытаемся извлечь timestamp для вычисления времени ответа
                size_t timestamp_pos = response.find("\"timestamp\":");
                if (timestamp_pos != std::string::npos) {
                    try {
                        size_t start = timestamp_pos + 12; // длина "timestamp":
                        size_t end = response.find_first_of(",}", start);
                        if (end != std::string::npos) {
                            std::string timestamp_str = response.substr(start, end - start);
                            long long sent_time = std::stoll(timestamp_str);
                            
                            auto current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                                receive_time.time_since_epoch()).count();
                            
                            double response_time = current_time - sent_time;
                            if (response_time > 0) {
                                std::lock_guard<std::mutex> lock(result_.result_mutex);
                                result_.response_times.push_back(response_time);
                            }
                        }
                    } catch (...) {
                        // Игнорируем ошибки парсинга timestamp
                    }
                }
                
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && running_) {
                std::lock_guard<std::mutex> lock(result_.result_mutex);
                result_.errors.push_back("Receive error: " + std::string(strerror(errno)));
            }
        }
    }
    
    void run_test() {
        if (!setup()) {
            return;
        }
        
        std::cout << "\n🚀 Starting stress test..." << std::endl;
        std::cout << "📊 Rate: " << rate_per_second_ << " req/sec" << std::endl;
        std::cout << "⏱️ Duration: " << duration_seconds_ << "s" << std::endl;
        std::cout << "🎯 Target: " << target_ip_ << ":" << target_port_ << std::endl;
        std::cout << std::string(50, '=') << std::endl;
        
        // Запускаем поток получения ответов
        std::thread receive_thread(&SimpleTester::receive_responses, this);
        
        // Отправка команд
        auto start_time = std::chrono::steady_clock::now();
        auto end_time = start_time + std::chrono::seconds(duration_seconds_);
        
        double interval = rate_per_second_ > 0 ? 1.0 / rate_per_second_ : 0;
        int request_id = 1;
        
        while (std::chrono::steady_clock::now() < end_time && running_) {
            send_command(request_id++);
            
            if (interval > 0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(interval));
            }
            
            // Показываем прогресс каждую секунду
            if (request_id % rate_per_second_ == 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                std::cout << "⏳ " << elapsed << "s elapsed, sent: " << result_.sent_count.load() 
                         << ", received: " << result_.received_count.load() << std::endl;
            }
        }
        
        // Ждем последние ответы
        std::cout << "⏳ Waiting for remaining responses..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        running_ = false;
        receive_thread.join();
        
        print_statistics();
    }
    
    void print_statistics() {
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "📊 STRESS TEST RESULTS" << std::endl;
        std::cout << std::string(50, '=') << std::endl;
        std::cout << "📤 Sent commands: " << result_.sent_count.load() << std::endl;
        std::cout << "📥 Received responses: " << result_.received_count.load() << std::endl;
        std::cout << "📈 Success rate: " << std::fixed << std::setprecision(2) 
                  << result_.success_rate() << "%" << std::endl;
        std::cout << "❌ Errors: " << result_.errors.size() << std::endl;
        
        // Статистика по времени ответа
        std::lock_guard<std::mutex> lock(result_.result_mutex);
        if (!result_.response_times.empty()) {
            auto times = result_.response_times;
            std::sort(times.begin(), times.end());
            
            double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
            double min_time = *std::min_element(times.begin(), times.end());
            double max_time = *std::max_element(times.begin(), times.end());
            double p95 = times[static_cast<size_t>(times.size() * 0.95)];
            
            std::cout << "\n⏱️ Response Time Statistics:" << std::endl;
            std::cout << "   Average: " << std::fixed << std::setprecision(2) << avg << "ms" << std::endl;
            std::cout << "   Minimum: " << min_time << "ms" << std::endl;
            std::cout << "   Maximum: " << max_time << "ms" << std::endl;
            std::cout << "   95th percentile: " << p95 << "ms" << std::endl;
        }
        
        // Показываем первые ошибки
        if (!result_.errors.empty()) {
            std::cout << "\n❌ First 5 errors:" << std::endl;
            for (size_t i = 0; i < std::min(result_.errors.size(), size_t(5)); ++i) {
                std::cout << "   - " << result_.errors[i] << std::endl;
            }
        }
        
        std::cout << std::string(50, '=') << std::endl;
    }
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --target-ip <ip>       Target IP address (default: 127.0.0.1)" << std::endl;
    std::cout << "  --target-port <port>   Target port (default: 11000)" << std::endl;
    std::cout << "  --response-port <port> Response port (default: 11001)" << std::endl;
    std::cout << "  --rate <req/sec>       Requests per second (default: 10)" << std::endl;
    std::cout << "  --duration <seconds>   Test duration in seconds (default: 5)" << std::endl;
    std::cout << "  --help                 Show this help" << std::endl;
    std::cout << "\nExamples:" << std::endl;
    std::cout << "  " << program_name << " --rate 100 --duration 10" << std::endl;
    std::cout << "  " << program_name << " --target-ip 192.168.1.100 --rate 50" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string target_ip = "127.0.0.1";
    int target_port = 11000;
    int response_port = 11001;
    int rate = 10;
    int duration = 5;
    
    // Парсим аргументы командной строки
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc && std::string(argv[i]) != "--help") {
            std::cerr << "❌ Missing value for " << argv[i] << std::endl;
            return 1;
        }
        
        std::string arg = argv[i];
        
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--target-ip") {
            target_ip = argv[i + 1];
        } else if (arg == "--target-port") {
            target_port = std::stoi(argv[i + 1]);
        } else if (arg == "--response-port") {
            response_port = std::stoi(argv[i + 1]);
        } else if (arg == "--rate") {
            rate = std::stoi(argv[i + 1]);
        } else if (arg == "--duration") {
            duration = std::stoi(argv[i + 1]);
        } else {
            std::cerr << "❌ Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }
    
    try {
        SimpleTester tester(target_ip, target_port, response_port, rate, duration);
        tester.run_test();
    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
