#include "images.hpp"

#include "net.hpp"
#include "util.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <minizip/unzip.h>
#include <turbojpeg.h>

#include <algorithm>
#include <vector>

enum { IMG_PENDING, IMG_LOADING, IMG_READY, IMG_FAILED };

static const size_t BUDGET = 56u * 1024 * 1024;
#define CACHE (APP_DIR + "/cache")

void Images::start()
{
    fs::mkdirs(CACHE);
    Images* self = this;
    for (int i = 0; i < 3; i++) {
        SceUID th = sceKernelCreateThread("vpkgj_images", worker, 0x10000100, 256 * 1024, 0, 0, nullptr);
        sceKernelStartThread(th, sizeof(self), &self);
        threads.push_back(th);
    }
}

void Images::stop()
{
    quit = true;
    for (auto th : threads) {
        SceUInt timeout = 3 * 1000 * 1000;
        sceKernelWaitThreadEnd(th, nullptr, &timeout);
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

static vita2d_texture* decode_jpeg(const uint8_t* data, size_t size)
{
    tjhandle h = tjInitDecompress();
    if (!h)
        return nullptr;
    int w = 0, hh = 0, ss = 0, cs = 0;
    vita2d_texture* tex = nullptr;
    if (tjDecompressHeader3(h, data, size, &w, &hh, &ss, &cs) == 0 && w > 0 && hh > 0) {
        int sw = w, sh = hh;
        int n = 0;
        tjscalingfactor* sf = tjGetScalingFactors(&n);
        for (int i = 0; i < n && (sw > 1024 || sh > 1024); i++) {
            int tw = TJSCALED(w, sf[i]), th = TJSCALED(hh, sf[i]);
            if (tw <= 1024 && th <= 1024 && tw < sw) {
                sw = tw;
                sh = th;
            }
        }
        tex = vita2d_create_empty_texture(sw, sh);
        if (tex) {
            int pitch = vita2d_texture_get_stride(tex);
            if (tjDecompress2(h, data, size, (unsigned char*)vita2d_texture_get_datap(tex), sw, pitch, sh, TJPF_RGBA, TJFLAG_FASTDCT) != 0 &&
                tjGetErrorCode(h) == TJERR_FATAL) {
                vita2d_free_texture(tex);
                tex = nullptr;
            }
        }
    }
    tjDestroy(h);
    return tex;
}

static vita2d_texture* load_file(const std::string& path)
{
    std::string data;
    if (!fs::read(path, data) || data.size() < 4)
        return nullptr;
    const uint8_t* b = (const uint8_t*)data.data();
    if (b[0] == 0xFF && b[1] == 0xD8)
        return decode_jpeg(b, data.size());
    if (b[0] == 0x89 && b[1] == 'P')
        return vita2d_load_PNG_buffer(b);
    if (b[0] == 'B' && b[1] == 'M')
        return vita2d_load_BMP_buffer(b);
    return nullptr;
}

static std::mutex pak_lock;

static vita2d_texture* load_pak(const std::string& name)
{
    std::vector<uint8_t> buf;
    {
        std::lock_guard<std::mutex> lock(pak_lock);
    static unzFile pak = nullptr;
    static bool tried = false;
    static std::map<std::string, unz64_file_pos> index;
    if (!tried) {
        tried = true;
        pak = unzOpen64("app0:covers.pak");
        if (pak && unzGoToFirstFile(pak) == UNZ_OK) {
            do {
                char n[256];
                unz64_file_pos pos;
                unz_file_info64 fi;
                if (unzGetCurrentFileInfo64(pak, &fi, n, sizeof(n), nullptr, 0, nullptr, 0) == UNZ_OK &&
                    unzGetFilePos64(pak, &pos) == UNZ_OK)
                    index[n] = pos;
            } while (unzGoToNextFile(pak) == UNZ_OK);
        }
    }
    auto it = index.find(name);
    if (!pak || it == index.end() || unzGoToFilePos64(pak, &it->second) != UNZ_OK)
        return nullptr;
    unz_file_info64 info;
    if (unzGetCurrentFileInfo64(pak, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK || info.uncompressed_size > 8u * 1024 * 1024)
        return nullptr;
    if (unzOpenCurrentFile(pak) != UNZ_OK)
        return nullptr;
    buf.resize(info.uncompressed_size);
    int r = unzReadCurrentFile(pak, buf.data(), buf.size());
    unzCloseCurrentFile(pak);
    if (r != (int)buf.size() || buf.size() < 4)
        return nullptr;
    }
    if (buf[0] == 0xFF && buf[1] == 0xD8)
        return decode_jpeg(buf.data(), buf.size());
    if (buf[0] == 0x89 && buf[1] == 'P')
        return vita2d_load_PNG_buffer(buf.data());
    return nullptr;
}

void Images::load(const std::string& url)
{
    if (starts_with(url, "pak:")) {
        vita2d_texture* tex = load_pak(url.substr(4));
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
        return;
    }
    bool local = !starts_with(url, "http://") && !starts_with(url, "https://");
    std::string path = local ? url : CACHE + "/" + hash_name(url);
    if (!local && !fs::exists(path)) {
        std::string err;
        if (!net::fetch(url, path, nullptr, err))
            log_line("image %s: %s", url.c_str(), err.c_str());
    }
    vita2d_texture* tex = load_file(path);
    if (!tex && !local)
        sceIoRemove(path.c_str());
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
