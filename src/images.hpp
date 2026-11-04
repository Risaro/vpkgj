#pragma once

#include "mutex.hpp"
#include "net.hpp"

#include <vita2d.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum ImgSize { IMG_THUMB, IMG_FULL };

struct Image {
    std::string url;
    vita2d_texture* tex = nullptr;
    int state = 0;
    int size = IMG_FULL;
    uint64_t used = 0;
    uint64_t seen = 0;
    uint64_t seq = 0;
    uint64_t ready_at = 0;
    size_t bytes = 0;
    net::Progress* progress = nullptr;
};

struct Preload {
    std::atomic<int> total{0};
    std::atomic<int> done{0};
    std::atomic<int> failed{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<int> running{0};
    std::atomic<bool> cancel{false};
    net::Progress live[3];
};

class Images {
public:
    void start();
    void stop();
    vita2d_texture* get(const std::string& url, float* alpha = nullptr, int size = IMG_FULL);
    vita2d_texture* peek(const std::string& url, int size);
    void prefetch(const std::string& url, int size = IMG_THUMB);
    void tick();
    void clear_disk();
    void set_busy(bool b) { busy = b; }
    void preload(const std::vector<std::string>& urls);
    void preload_stop();
    bool cached(const std::string& url);
    Preload pre;

private:
    Mutex m;
    std::unordered_map<std::string, Image> cache;
    std::unordered_map<std::string, uint64_t> retry_at;
    std::unordered_map<std::string, int> fails;
    std::vector<vita2d_texture*> to_free;
    uint64_t frame = 0;
    uint64_t seq = 0;
    size_t total = 0;
    std::vector<int> threads;
    std::atomic<bool> quit{false};
    std::atomic<bool> busy{false};
    static int decode_worker(unsigned int, void* arg);
    static int net_worker(unsigned int, void* arg);
    static int preload_worker(unsigned int, void* arg);
    Mutex pre_m;
    std::vector<std::string> pre_urls;
    size_t pre_next = 0;
    std::vector<int> pre_threads;
    Image* request(const std::string& url, int size, bool visible);
    bool pick(int from, int to, std::string& key, Image& out);
    void finish(const std::string& key, vita2d_texture* tex);
    void fail(const std::string& key, bool permanent);
};

void images_close_pak();
