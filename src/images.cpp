#include "images.hpp"

#include "util.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include <minizip/unzip.h>
#include <png.h>
#include <turbojpeg.h>

#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <set>
#include <vector>

enum { IMG_PENDING, IMG_DECODING, IMG_NET, IMG_FETCHING, IMG_READY, IMG_FAILED };

static Mutex gpu_lock;

static const size_t BUDGET = 40u * 1024 * 1024;
static const int NET_THREADS = 2;
static const int DECODE_THREADS = 2;
#define CACHE (APP_DIR + "/cache")

struct Box {
    int w, h;
};

static Box box_for(int size)
{
    return size == IMG_THUMB ? Box{288, 162} : Box{960, 544};
}

static std::string key_of(const std::string& url, int size)
{
    return (size == IMG_THUMB ? "t" : "f") + url;
}

static bool remote(const std::string& url)
{
    return starts_with(url, "http://") || starts_with(url, "https://");
}

static std::string disk_path(const std::string& url)
{
    return CACHE + "/" + hash_name(url);
}

static vita2d_texture* new_texture(int w, int h)
{
    std::lock_guard<Mutex> g(gpu_lock);
    return vita2d_create_empty_texture(w, h);
}

static void free_texture(vita2d_texture* t)
{
    std::lock_guard<Mutex> g(gpu_lock);
    vita2d_free_texture(t);
}

static vita2d_texture* decode_jpeg(const uint8_t* data, size_t size, Box box)
{
    tjhandle h = tjInitDecompress();
    if (!h)
        return nullptr;
    int w = 0, hh = 0, ss = 0, cs = 0;
    vita2d_texture* tex = nullptr;
    if (tjDecompressHeader3(h, data, size, &w, &hh, &ss, &cs) == 0 && w > 0 && hh > 0 && w <= 8192 && hh <= 8192) {
        int sw = w, sh = hh;
        int n = 0;
        tjscalingfactor* sf = tjGetScalingFactors(&n);
        for (int i = 0; i < n; i++) {
            int tw = TJSCALED(w, sf[i]), th = TJSCALED(hh, sf[i]);
            if (tw < sw && tw >= std::min(w, box.w) && th >= std::min(hh, box.h)) {
                sw = tw;
                sh = th;
            }
        }
        while ((sw > 2048 || sh > 2048)) {
            bool found = false;
            for (int i = 0; i < n; i++) {
                int tw = TJSCALED(w, sf[i]), th = TJSCALED(hh, sf[i]);
                if (tw < sw) {
                    sw = tw;
                    sh = th;
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        }
        tex = new_texture(sw, sh);
        if (tex) {
            int pitch = vita2d_texture_get_stride(tex);
            if (tjDecompress2(h, data, size, (unsigned char*)vita2d_texture_get_datap(tex), sw, pitch, sh, TJPF_RGBA, TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE) != 0 &&
                tjGetErrorCode(h) == TJERR_FATAL) {
                free_texture(tex);
                tex = nullptr;
            }
        }
    }
    tjDestroy(h);
    return tex;
}

struct PngSrc {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

static void png_read(png_structp p, png_bytep out, png_size_t n)
{
    PngSrc* s = (PngSrc*)png_get_io_ptr(p);
    if (s->pos + n > s->size)
        png_error(p, "eof");
    memcpy(out, s->data + s->pos, n);
    s->pos += n;
}

static vita2d_texture* decode_png(const uint8_t* data, size_t size, Box box)
{
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!p)
        return nullptr;
    png_infop info = png_create_info_struct(p);
    if (!info) {
        png_destroy_read_struct(&p, nullptr, nullptr);
        return nullptr;
    }
    std::vector<uint8_t> pixels;
    std::vector<png_bytep> rows;
    vita2d_texture* tex = nullptr;
    PngSrc src{data, size, 0};
    if (setjmp(png_jmpbuf(p))) {
        png_destroy_read_struct(&p, &info, nullptr);
        if (tex)
            free_texture(tex);
        return nullptr;
    }
    png_set_read_fn(p, &src, png_read);
    png_read_info(p, info);
    png_uint_32 w = png_get_image_width(p, info), h = png_get_image_height(p, info);
    int depth = png_get_bit_depth(p, info), type = png_get_color_type(p, info);
    if (!w || !h || (uint64_t)w * h > 6u * 1024 * 1024) {
        png_destroy_read_struct(&p, &info, nullptr);
        return nullptr;
    }
    if (type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(p);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(p);
    if (png_get_valid(p, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(p);
    if (depth == 16)
        png_set_strip_16(p);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(p);
    png_set_filler(p, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(p);
    png_read_update_info(p, info);
    if (png_get_rowbytes(p, info) != (size_t)w * 4 || png_get_channels(p, info) != 4) {
        png_destroy_read_struct(&p, &info, nullptr);
        return nullptr;
    }
    pixels.resize((size_t)w * h * 4);
    rows.resize(h);
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = pixels.data() + (size_t)y * w * 4;
    png_read_image(p, rows.data());
    png_read_end(p, nullptr);
    png_destroy_read_struct(&p, &info, nullptr);

    int k = (int)std::min(w / (png_uint_32)std::max(1, box.w), h / (png_uint_32)std::max(1, box.h));
    if (k < 1)
        k = 1;
    while (w / k > 2048 || h / k > 2048)
        k++;
    int ow = w / k, oh = h / k;
    tex = new_texture(ow, oh);
    if (!tex)
        return nullptr;
    int stride = vita2d_texture_get_stride(tex);
    uint8_t* dst = (uint8_t*)vita2d_texture_get_datap(tex);
    if (k == 1) {
        for (int y = 0; y < oh; y++)
            memcpy(dst + (size_t)y * stride, rows[y], (size_t)ow * 4);
        return tex;
    }
    std::vector<uint32_t> acc((size_t)ow * 4);
    std::vector<uint8_t> line((size_t)ow * 4);
    uint32_t div = k * k;
    for (int oy = 0; oy < oh; oy++) {
        std::fill(acc.begin(), acc.end(), 0);
        for (int dy = 0; dy < k; dy++) {
            const uint8_t* r = rows[oy * k + dy];
            for (int ox = 0; ox < ow; ox++) {
                const uint8_t* s = r + (size_t)ox * k * 4;
                uint32_t* a = &acc[(size_t)ox * 4];
                for (int dx = 0; dx < k; dx++, s += 4) {
                    a[0] += s[0];
                    a[1] += s[1];
                    a[2] += s[2];
                    a[3] += s[3];
                }
            }
        }
        for (size_t i = 0; i < line.size(); i++)
            line[i] = (uint8_t)(acc[i] / div);
        memcpy(dst + (size_t)oy * stride, line.data(), line.size());
    }
    return tex;
}

static vita2d_texture* decode(const uint8_t* b, size_t n, Box box)
{
    if (n < 8)
        return nullptr;
    if (b[0] == 0xFF && b[1] == 0xD8)
        return decode_jpeg(b, n, box);
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
        return decode_png(b, n, box);
    return nullptr;
}

static Mutex pak_lock;
static unzFile pak = nullptr;
static bool pak_tried = false;
static std::unordered_map<std::string, unz64_file_pos> pak_index;

void images_close_pak()
{
    std::lock_guard<Mutex> lock(pak_lock);
    if (pak)
        unzClose(pak);
    pak = nullptr;
    pak_tried = true;
    pak_index.clear();
}

static bool read_pak(const std::string& name, std::vector<uint8_t>& buf)
{
    std::lock_guard<Mutex> lock(pak_lock);
    if (!pak_tried) {
        pak_tried = true;
        pak = unzOpen64("app0:covers.pak");
        if (pak && unzGoToFirstFile(pak) == UNZ_OK) {
            do {
                char n[256];
                unz64_file_pos pos;
                unz_file_info64 fi;
                if (unzGetCurrentFileInfo64(pak, &fi, n, sizeof(n), nullptr, 0, nullptr, 0) == UNZ_OK &&
                    unzGetFilePos64(pak, &pos) == UNZ_OK)
                    pak_index[n] = pos;
            } while (unzGoToNextFile(pak) == UNZ_OK);
        }
    }
    auto it = pak_index.find(name);
    if (!pak || it == pak_index.end() || unzGoToFilePos64(pak, &it->second) != UNZ_OK)
        return false;
    unz_file_info64 info;
    if (unzGetCurrentFileInfo64(pak, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK || info.uncompressed_size > 8u * 1024 * 1024)
        return false;
    if (unzOpenCurrentFile(pak) != UNZ_OK)
        return false;
    buf.resize(info.uncompressed_size);
    int r = unzReadCurrentFile(pak, buf.data(), buf.size());
    unzCloseCurrentFile(pak);
    return r == (int)buf.size();
}

static bool read_file(const std::string& path, std::vector<uint8_t>& buf)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0)
        return false;
    int64_t len = sceIoLseek(f, 0, SCE_SEEK_END);
    sceIoLseek(f, 0, SCE_SEEK_SET);
    if (len <= 0 || len > 24 * 1024 * 1024) {
        sceIoClose(f);
        return false;
    }
    buf.resize((size_t)len);
    size_t pos = 0;
    while (pos < buf.size()) {
        int r = sceIoRead(f, buf.data() + pos, buf.size() - pos);
        if (r <= 0)
            break;
        pos += r;
    }
    sceIoClose(f);
    return pos == buf.size();
}

void Images::start()
{
    fs::mkdirs(CACHE);
    quit = false;
    Images* self = this;
    for (int i = 0; i < DECODE_THREADS; i++) {
        SceUID th = sceKernelCreateThread("vpkgj_decode", decode_worker, 180, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (th >= 0) {
            sceKernelStartThread(th, sizeof(self), &self);
            threads.push_back(th);
        }
    }
    for (int i = 0; i < NET_THREADS; i++) {
        struct Arg {
            Images* self;
            int index;
        } arg{self, i};
        SceUID th = sceKernelCreateThread("vpkgj_imgnet", net_worker, 185, 256 * 1024, 0, i == 0 ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_1, nullptr);
        if (th >= 0) {
            sceKernelStartThread(th, sizeof(arg), &arg);
            threads.push_back(th);
        }
    }
}

void Images::stop()
{
    quit = true;
    preload_stop();
    {
        std::lock_guard<Mutex> lock(m);
        for (auto& kv : cache)
            if (kv.second.progress)
                kv.second.progress->cancel = true;
    }
    for (auto th : threads) {
        SceUInt timeout = 3 * 1000 * 1000;
        sceKernelWaitThreadEnd(th, nullptr, &timeout);
    }
    threads.clear();
    vita2d_wait_rendering_done();
    for (auto& kv : cache)
        if (kv.second.tex)
            vita2d_free_texture(kv.second.tex);
    for (auto t : to_free)
        vita2d_free_texture(t);
    cache.clear();
    to_free.clear();
    total = 0;
    images_close_pak();
}

Image* Images::request(const std::string& url, int size, bool visible)
{
    std::string key = key_of(url, size);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto r = retry_at.find(key);
        if (r != retry_at.end() && now_ms() < r->second)
            return nullptr;
        Image img;
        img.url = url;
        img.size = size;
        img.state = IMG_PENDING;
        it = cache.emplace(key, img).first;
    }
    Image& img = it->second;
    img.used = frame;
    if (visible) {
        img.seen = frame;
        img.seq = seq++;
    }
    return &img;
}

vita2d_texture* Images::get(const std::string& url, float* alpha, int size)
{
    if (alpha)
        *alpha = 0;
    if (url.empty())
        return nullptr;
    std::lock_guard<Mutex> lock(m);
    Image* img = request(url, size, true);
    if (!img || img->state != IMG_READY)
        return nullptr;
    if (alpha) {
        float a = (now_ms() - img->ready_at) / 200.0f;
        *alpha = a > 1 ? 1 : a;
    }
    return img->tex;
}

vita2d_texture* Images::peek(const std::string& url, int size)
{
    if (url.empty())
        return nullptr;
    std::lock_guard<Mutex> lock(m);
    auto it = cache.find(key_of(url, size));
    if (it == cache.end() || it->second.state != IMG_READY)
        return nullptr;
    it->second.used = frame;
    return it->second.tex;
}

void Images::prefetch(const std::string& url, int size)
{
    if (url.empty())
        return;
    std::lock_guard<Mutex> lock(m);
    request(url, size, false);
}

bool Images::pick(int from, int to, std::string& key, Image& out)
{
    std::lock_guard<Mutex> lock(m);
    Image* best = nullptr;
    const std::string* best_key = nullptr;
    std::vector<const std::string*> busy_urls;
    if (from == IMG_NET)
        for (auto& kv : cache)
            if (kv.second.state == IMG_FETCHING)
                busy_urls.push_back(&kv.second.url);
    for (auto it = cache.begin(); it != cache.end();) {
        Image& img = it->second;
        if ((img.state == IMG_PENDING || img.state == IMG_NET) && img.used + 20 < frame) {
            it = cache.erase(it);
            continue;
        }
        bool taken = false;
        for (auto u : busy_urls)
            if (*u == img.url)
                taken = true;
        if (img.state == from && !taken) {
            bool better = !best;
            if (best) {
                bool v1 = img.seen + 2 >= frame, v2 = best->seen + 2 >= frame;
                if (v1 != v2)
                    better = v1;
                else if (img.used != best->used)
                    better = img.used > best->used;
                else
                    better = img.seq < best->seq;
            }
            if (better) {
                best = &img;
                best_key = &it->first;
            }
        }
        ++it;
    }
    if (!best)
        return false;
    best->state = to;
    key = *best_key;
    out = *best;
    return true;
}

void Images::finish(const std::string& key, vita2d_texture* tex)
{
    if (tex)
        vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    std::lock_guard<Mutex> lock(m);
    auto it = cache.find(key);
    if (it == cache.end() || it->second.state != IMG_DECODING) {
        if (tex)
            to_free.push_back(tex);
        return;
    }
    Image& img = it->second;
    img.tex = tex;
    img.bytes = vita2d_texture_get_stride(tex) * vita2d_texture_get_height(tex);
    img.ready_at = now_ms();
    img.state = IMG_READY;
    total += img.bytes;
    retry_at.erase(key);
    fails.erase(key);
}

void Images::fail(const std::string& key, bool permanent)
{
    std::lock_guard<Mutex> lock(m);
    int n = ++fails[key];
    uint64_t wait = permanent ? 3600ull * 1000 : std::min<uint64_t>(120000, 3000ull << std::min(n, 6));
    retry_at[key] = now_ms() + wait;
    cache.erase(key);
}

int Images::decode_worker(unsigned int, void* arg)
{
    Images* self = *(Images**)arg;
    std::vector<uint8_t> buf;
    while (!self->quit) {
        std::string key;
        Image job;
        if (!self->pick(IMG_PENDING, IMG_DECODING, key, job)) {
            sceKernelDelayThread(8 * 1000);
            continue;
        }
        Box box = box_for(job.size);
        bool have = false;
        bool net = false;
        if (starts_with(job.url, "pak:")) {
            have = read_pak(job.url.substr(4), buf);
        } else if (remote(job.url)) {
            have = read_file(disk_path(job.url), buf);
            net = !have;
        } else {
            have = read_file(job.url, buf);
        }
        if (net) {
            std::lock_guard<Mutex> lock(self->m);
            auto it = self->cache.find(key);
            if (it != self->cache.end())
                it->second.state = IMG_NET;
            continue;
        }
        vita2d_texture* tex = have ? decode(buf.data(), buf.size(), box) : nullptr;
        if (buf.capacity() > 4 * 1024 * 1024)
            std::vector<uint8_t>().swap(buf);
        if (!tex) {
            if (have && remote(job.url))
                sceIoRemove(disk_path(job.url).c_str());
            self->fail(key, !remote(job.url));
            continue;
        }
        self->finish(key, tex);
    }
    return sceKernelExitDeleteThread(0);
}

int Images::net_worker(unsigned int, void* arg)
{
    struct Arg {
        Images* self;
        int index;
    } a = *(Arg*)arg;
    Images* self = a.self;
    net::Session session;
    session.buffer = 64 * 1024;
    while (!self->quit) {
        if (a.index > 0 && self->busy) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        std::string key;
        Image job;
        if (!self->pick(IMG_NET, IMG_FETCHING, key, job)) {
            sceKernelDelayThread(15 * 1000);
            continue;
        }
        net::Progress progress;
        {
            std::lock_guard<Mutex> lock(self->m);
            auto it = self->cache.find(key);
            if (it != self->cache.end())
                it->second.progress = &progress;
        }
        std::string err;
        bool ok = net::fetch(job.url, disk_path(job.url), &progress, err, &session);
        bool canceled = progress.cancel;
        {
            std::lock_guard<Mutex> lock(self->m);
            auto it = self->cache.find(key);
            if (it != self->cache.end()) {
                it->second.progress = nullptr;
                if (ok)
                    it->second.state = IMG_PENDING;
                else if (canceled)
                    self->cache.erase(it);
            }
        }
        if (!ok && !canceled) {
            if (!starts_with(err, "HTTP 4"))
                log_line("image %s: %s", job.url.c_str(), err.c_str());
            self->fail(key, starts_with(err, "HTTP 4"));
        }
        if (self->busy)
            sceKernelDelayThread(30 * 1000);
    }
    return sceKernelExitDeleteThread(0);
}

void Images::tick()
{
    std::vector<vita2d_texture*> victims;
    {
        std::lock_guard<Mutex> lock(m);
        frame++;
        victims.swap(to_free);
        for (auto& kv : cache)
            if (kv.second.state == IMG_FETCHING && kv.second.progress && kv.second.used + 90 < frame)
                kv.second.progress->cancel = true;
        if (frame % 30 == 0) {
            for (auto it = cache.begin(); it != cache.end();) {
                Image& img = it->second;
                if (img.state == IMG_READY && img.used + 300 < frame && total > BUDGET / 3) {
                    victims.push_back(img.tex);
                    total -= img.bytes;
                    it = cache.erase(it);
                } else {
                    ++it;
                }
            }
            if (retry_at.size() > 512) {
                uint64_t now = now_ms();
                for (auto it = retry_at.begin(); it != retry_at.end();)
                    it = it->second < now ? retry_at.erase(it) : std::next(it);
            }
        }
        if (total > BUDGET) {
            std::vector<std::pair<uint64_t, std::string>> ready;
            for (auto& kv : cache)
                if (kv.second.state == IMG_READY && kv.second.used + 2 < frame)
                    ready.push_back({kv.second.used, kv.first});
            std::sort(ready.begin(), ready.end());
            for (auto& r : ready) {
                if (total <= BUDGET * 3 / 4)
                    break;
                auto it = cache.find(r.second);
                victims.push_back(it->second.tex);
                total -= it->second.bytes;
                cache.erase(it);
            }
        }
    }
    if (!victims.empty()) {
        vita2d_wait_rendering_done();
        std::lock_guard<Mutex> g(gpu_lock);
        for (auto t : victims)
            vita2d_free_texture(t);
    }
}

bool Images::cached(const std::string& url)
{
    return !remote(url) || fs::exists(disk_path(url));
}

void Images::preload(const std::vector<std::string>& urls)
{
    preload_stop();
    {
        std::lock_guard<Mutex> lock(pre_m);
        pre_urls.clear();
        std::set<std::string> seen;
        for (auto& u : urls)
            if (remote(u) && seen.insert(u).second)
                pre_urls.push_back(u);
        pre_next = 0;
    }
    pre.total = (int)pre_urls.size();
    pre.done = 0;
    pre.failed = 0;
    pre.bytes = 0;
    pre.cancel = false;
    for (auto& l : pre.live) {
        l.done = 0;
        l.total = 0;
        l.cancel = false;
    }
    if (pre_urls.empty())
        return;
    fs::mkdirs(CACHE);
    for (int i = 0; i < 3; i++) {
        struct Arg {
            Images* self;
            int index;
        } arg{this, i};
        SceUID th = sceKernelCreateThread("vpkgj_preload", preload_worker, 186, 256 * 1024, 0, i == 1 ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_1, nullptr);
        if (th >= 0) {
            pre.running++;
            sceKernelStartThread(th, sizeof(arg), &arg);
            pre_threads.push_back(th);
        }
    }
}

void Images::preload_stop()
{
    pre.cancel = true;
    for (auto& l : pre.live)
        l.cancel = true;
    for (auto th : pre_threads) {
        SceUInt timeout = 5 * 1000 * 1000;
        sceKernelWaitThreadEnd(th, nullptr, &timeout);
    }
    pre_threads.clear();
}

int Images::preload_worker(unsigned int, void* arg)
{
    struct Arg {
        Images* self;
        int index;
    } a = *(Arg*)arg;
    Images* self = a.self;
    Preload& p = self->pre;
    net::Progress& live = p.live[a.index];
    net::Session session;
    session.buffer = 64 * 1024;
    while (!p.cancel && !self->quit) {
        std::string url;
        {
            std::lock_guard<Mutex> lock(self->pre_m);
            if (self->pre_next >= self->pre_urls.size())
                break;
            url = self->pre_urls[self->pre_next++];
        }
        std::string path = disk_path(url);
        if (fs::exists(path)) {
            p.done++;
            continue;
        }
        std::string tmp = path + ".pre" + std::to_string(a.index);
        std::string err;
        live.done = 0;
        live.total = 0;
        bool ok = net::fetch(url, tmp, &live, err, &session);
        if (p.cancel) {
            sceIoRemove(tmp.c_str());
            sceIoRemove((tmp + ".part").c_str());
            break;
        }
        if (ok) {
            int64_t n = fs::size(tmp);
            if (fs::exists(path) || sceIoRename(tmp.c_str(), path.c_str()) < 0)
                sceIoRemove(tmp.c_str());
            p.bytes += n > 0 ? (uint64_t)n : 0;
        } else {
            sceIoRemove((tmp + ".part").c_str());
            p.failed++;
        }
        live.done = 0;
        live.total = 0;
        p.done++;
    }
    live.done = 0;
    p.running--;
    return sceKernelExitDeleteThread(0);
}

void Images::clear_disk()
{
    fs::rmtree(CACHE);
    fs::mkdirs(CACHE);
    std::lock_guard<Mutex> lock(m);
    retry_at.clear();
    fails.clear();
}
