#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace net {

struct Progress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
};

using Sink = std::function<bool(const uint8_t* data, size_t size)>;

struct Session {
    void* handle = nullptr;
    long buffer = 512 * 1024;
    ~Session();
};

void init();
void fini();
void set_speed_limit(uint64_t bytes_per_sec);
bool get(const std::string& url, std::string& out, std::string& err, Session* s = nullptr);
bool fetch(const std::string& url, const std::string& path, Progress* p, std::string& err, Session* s = nullptr);
bool stream(const std::string& url, const Sink& sink, Progress* p, std::string& err);
bool get_range(const std::string& url, const std::string& range, std::string& out, std::string& err);

}
