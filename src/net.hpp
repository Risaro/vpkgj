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

void init();
void fini();
bool get(const std::string& url, std::string& out, std::string& err);
bool fetch(const std::string& url, const std::string& path, Progress* p, std::string& err);
bool stream(const std::string& url, const Sink& sink, Progress* p, std::string& err);

}
