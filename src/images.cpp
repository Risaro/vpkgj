#include "images.hpp"

#include "net.hpp"
#include "util.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>

enum { IMG_PENDING, IMG_LOADING, IMG_READY, IMG_FAILED };

static const size_t BUDGET = 56u * 1024 * 1024;
#define CACHE (APP_DIR + "/cache")

void Images::start()
{
    fs::mkdirs(CACHE);
    thread = sceKernelCreateThread("vpkgj_images", worker, 0x10000100, 128 * 1024, 0, 0, nullptr);
    Images* self = this;
    sceKernelStartThread(thread, sizeof(self), &self);
}

void Images::stop()
{
    quit = true;
    if (thread >= 0) {
        SceUInt timeout = 3 * 1000 * 1000;
        sceKernelWaitThreadEnd(thread, nullptr, &timeout);
    }
    vita2d_wait_rendering_done();
    for (auto& kv : cache)
        if (kv.second.tex)
            vita2d_free_texture(kv.second.tex);
    for (auto t : to_free)
        vita2d_free_texture(t);
    cache.clear();
    to_free.clear();
}

vita2d_texture* Images::get(const std::string& url, float* alpha)
{
    if (alpha)
        *alpha = 0;
    if (url.empty())
        return nullptr;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(url);
    if (it == cache.end()) {
        Image img;
        img.state = IMG_PENDING;
        img.used = frame;
        cache[url] = img;
        queue.push_front(url);
        while (queue.size() > 48) {
            auto drop = cache.find(queue.back());
            if (drop != cache.end() && drop->second.state == IMG_PENDING)
                cache.erase(drop);
            queue.pop_back();
        }
        return nullptr;
    }
    Image& img = it->second;
    img.used = frame;
    if (img.state == IMG_PENDING) {
        auto q = std::find(queue.begin(), queue.end(), url);
        if (q != queue.end() && q != queue.begin()) {
            queue.erase(q);
            queue.push_front(url);
        }
    }
    if (img.state != IMG_READY)
        return nullptr;
    if (alpha) {
        float a = (now_ms() - img.ready_at) / 220.0f;
        *alpha = a > 1 ? 1 : a;
    }
    return img.tex;
}

void Images::tick()
{
    std::vector<vita2d_texture*> victims;
    {
        std::lock_guard<std::mutex> lock(m);
        frame++;
        victims.swap(to_free);
        if (total > BUDGET) {
            std::vector<std::pair<uint64_t, std::string>> ready;
            for (auto& kv : cache)
                if (kv.second.state == IMG_READY && kv.second.used + 2 < frame)
                    ready.push_back({kv.second.used, kv.first});
            std::sort(ready.begin(), ready.end());
            for (auto& r : ready) {
                if (total <= BUDGET * 3 / 4)
                    break;
                Image& img = cache[r.second];
                victims.push_back(img.tex);
                total -= img.bytes;
                cache.erase(r.second);
            }
        }
    }
    if (!victims.empty()) {
        vita2d_wait_rendering_done();
        for (auto t : victims)
            vita2d_free_texture(t);
    }
}

void Images::clear_disk()
{
    fs::rmtree(CACHE);
    fs::mkdirs(CACHE);
}

void Images::load(const std::string& url)
{
    bool local = !starts_with(url, "http://") && !starts_with(url, "https://");
    std::string path = local ? url : CACHE + "/" + hash_name(url);
    if (!local && !fs::exists(path)) {
        std::string err;
        if (!net::fetch(url, path, nullptr, err))
            log_line("image %s: %s", url.c_str(), err.c_str());
    }
    vita2d_texture* tex = nullptr;
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f >= 0) {
        unsigned char b[4] = {0};
        sceIoRead(f, b, 4);
        sceIoClose(f);
        if (b[0] == 0x89 && b[1] == 'P')
            tex = vita2d_load_PNG_file(path.c_str());
        else if (b[0] == 0xFF && b[1] == 0xD8)
            tex = vita2d_load_JPEG_file(path.c_str());
        else if (b[0] == 'B' && b[1] == 'M')
            tex = vita2d_load_BMP_file(path.c_str());
        if (!tex && !local)
            sceIoRemove(path.c_str());
    }
    if (tex)
        vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(url);
    if (it == cache.end()) {
        if (tex)
            to_free.push_back(tex);
        return;
    }
    if (!tex) {
        it->second.state = IMG_FAILED;
        return;
    }
    it->second.tex = tex;
    it->second.bytes = vita2d_texture_get_stride(tex) * vita2d_texture_get_height(tex);
    it->second.ready_at = now_ms();
    it->second.state = IMG_READY;
    total += it->second.bytes;
}

int Images::worker(unsigned int, void* arg)
{
    Images* self = *(Images**)arg;
    while (!self->quit) {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(self->m);
            while (!self->queue.empty()) {
                std::string u = self->queue.front();
                self->queue.pop_front();
                auto it = self->cache.find(u);
                if (it != self->cache.end() && it->second.state == IMG_PENDING) {
                    it->second.state = IMG_LOADING;
                    url = u;
                    break;
                }
            }
        }
        if (url.empty()) {
            sceKernelDelayThread(30 * 1000);
            continue;
        }
        self->load(url);
    }
    return sceKernelExitDeleteThread(0);
}
