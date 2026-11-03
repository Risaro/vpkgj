#pragma once

#include <vita2d.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct Image {
    vita2d_texture* tex = nullptr;
    int state = 0;
    uint64_t used = 0;
    uint64_t ready_at = 0;
    size_t bytes = 0;
};

class Images {
public:
    void start();
    void stop();
    vita2d_texture* get(const std::string& url, float* alpha = nullptr);
    void tick();
    void clear_disk();

private:
    std::mutex m;
    std::map<std::string, Image> cache;
    std::deque<std::string> queue;
    std::vector<vita2d_texture*> to_free;
    uint64_t frame = 0;
    size_t total = 0;
    std::vector<int> threads;
    std::atomic<bool> quit{false};
    static int worker(unsigned int, void* arg);
    void load(const std::string& url);
};
