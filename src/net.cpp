#include "net.hpp"

#include "util.hpp"

#include <curl/curl.h>
#include <psp2/io/fcntl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <sys/socket.h>
#include <vector>

static std::mutex resolve_lock;

extern "C" int __real_getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo**);
extern "C" void __real_freeaddrinfo(struct addrinfo*);

extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res)
{
    std::lock_guard<std::mutex> lock(resolve_lock);
    return __real_getaddrinfo(node, service, hints, res);
}

extern "C" void __wrap_freeaddrinfo(struct addrinfo* ai)
{
    std::lock_guard<std::mutex> lock(resolve_lock);
    __real_freeaddrinfo(ai);
}

extern "C" int __wrap_fcntl(int, int cmd, ...)
{
    if (cmd == F_SETFD)
        return 0;
    errno = ENOSYS;
    return -1;
}

namespace net {

static uint8_t netmem[4 * 1024 * 1024];
static CURLSH* share = nullptr;
static std::mutex share_locks[CURL_LOCK_DATA_LAST];
static std::atomic<uint64_t> speed_limit{0};

static void share_lock(CURL*, curl_lock_data d, curl_lock_access, void*)
{
    share_locks[d].lock();
}

static void share_unlock(CURL*, curl_lock_data d, void*)
{
    share_locks[d].unlock();
}

Session::~Session()
{
    if (handle)
        curl_easy_cleanup((CURL*)handle);
}

void init()
{
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p;
    p.memory = netmem;
    p.size = sizeof(netmem);
    p.flags = 0;
    sceNetInit(&p);
    sceNetCtlInit();
    curl_global_init(CURL_GLOBAL_ALL);
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
}

void set_speed_limit(uint64_t bps)
{
    speed_limit = bps;
}

void fini()
{
    if (share)
        curl_share_cleanup(share);
    share = nullptr;
    curl_global_cleanup();
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

struct Ctx {
    Progress* progress = nullptr;
    uint64_t offset = 0;
    std::function<bool(const uint8_t*, size_t)> sink;
    bool failed = false;
};

static size_t on_write(char* ptr, size_t size, size_t n, void* ud)
{
    Ctx* c = (Ctx*)ud;
    size_t len = size * n;
    if (!c->sink((const uint8_t*)ptr, len)) {
        c->failed = true;
        return 0;
    }
    return len;
}

static int on_progress(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    Ctx* c = (Ctx*)ud;
    if (!c->progress)
        return 0;
    if (dltotal > 0)
        c->progress->total = c->offset + dltotal;
    c->progress->done = c->offset + dlnow;
    return c->progress->cancel ? 1 : 0;
}

static int on_socket(void*, curl_socket_t fd, curlsocktype)
{
    int size = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    return CURL_SOCKOPT_OK;
}

static CURL* make(const std::string& url, Ctx& ctx, Session* s = nullptr)
{
    CURL* h;
    if (s && s->handle) {
        h = (CURL*)s->handle;
        curl_easy_reset(h);
    } else {
        h = curl_easy_init();
        if (s)
            s->handle = h;
    }
    if (share)
        curl_easy_setopt(h, CURLOPT_SHARE, share);
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "VPKGJ/1.0 (PS Vita)");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, s ? s->buffer : 512L * 1024);
    curl_easy_setopt(h, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    curl_easy_setopt(h, CURLOPT_SOCKOPTFUNCTION, on_socket);
    if (!s && speed_limit)
        curl_easy_setopt(h, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)speed_limit.load());
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    return h;
}

static bool is_html(CURL* h)
{
    char* ct = nullptr;
    curl_easy_getinfo(h, CURLINFO_CONTENT_TYPE, &ct);
    return ct && strstr(ct, "text/html");
}

static bool is_drive(const std::string& url)
{
    return url.find("drive.google.com") != std::string::npos ||
           url.find("drive.usercontent.google.com") != std::string::npos ||
           url.find("docs.google.com") != std::string::npos;
}

static std::string attr(const std::string& tag, const std::string& name)
{
    std::string key = name + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos)
        return "";
    p += key.size();
    size_t e = tag.find('"', p);
    if (e == std::string::npos)
        return "";
    std::string v = tag.substr(p, e - p);
    size_t a;
    while ((a = v.find("&amp;")) != std::string::npos)
        v.replace(a, 5, "&");
    return v;
}

static std::string drive_confirm(const std::string& html)
{
    size_t f = html.find("<form");
    if (f == std::string::npos)
        return "";
    size_t fe = html.find('>', f);
    size_t end = html.find("</form>", f);
    if (fe == std::string::npos || end == std::string::npos)
        return "";
    std::string action = attr(html.substr(f, fe - f), "action");
    if (action.empty())
        return "";
    std::string q;
    size_t p = fe;
    while ((p = html.find("<input", p)) != std::string::npos && p < end) {
        size_t e = html.find('>', p);
        std::string tag = html.substr(p, e - p);
        p = e;
        if (attr(tag, "type") != "hidden")
            continue;
        std::string n = attr(tag, "name");
        if (n.empty())
            continue;
        q += (q.empty() ? "?" : "&") + n + "=" + attr(tag, "value");
    }
    return q.empty() ? "" : action + q;
}

static std::string curl_err(CURL* h, CURLcode rc)
{
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    if (rc == CURLE_HTTP_RETURNED_ERROR)
        return "HTTP " + std::to_string(code);
    if (rc == CURLE_ABORTED_BY_CALLBACK)
        return "Отменено";
    return curl_easy_strerror(rc);
}

static void release(CURL* h, Session* s)
{
    if (!s)
        curl_easy_cleanup(h);
}

bool get(const std::string& url, std::string& out, std::string& err, Session* s)
{
    std::string target = url;
    for (int attempt = 0; attempt < 2; attempt++) {
        Ctx ctx;
        out.clear();
        ctx.sink = [&](const uint8_t* d, size_t n) {
            out.append((const char*)d, n);
            return out.size() < 64u * 1024 * 1024;
        };
        CURL* h = make(target, ctx, s);
        CURLcode rc = curl_easy_perform(h);
        bool html = is_html(h);
        if (rc != CURLE_OK) {
            err = curl_err(h, rc);
            release(h, s);
            return false;
        }
        release(h, s);
        if (html && is_drive(target)) {
            std::string next = drive_confirm(out);
            if (next.empty()) {
                err = "Google Drive не отдал файл, возможно лимит скачиваний";
                return false;
            }
            target = next;
            continue;
        }
        return true;
    }
    err = "Google Drive не отдал файл";
    return false;
}

struct Writer {
    SceUID f = -1;
    std::vector<uint8_t> buf;
    size_t used = 0;
    bool ok = true;

    explicit Writer(size_t cap) : buf(cap) {}
    ~Writer() { close(); }

    bool open(const std::string& path, bool append)
    {
        close();
        ok = true;
        f = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | (append ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
        return f >= 0;
    }

    bool flush()
    {
        size_t pos = 0;
        while (ok && pos < used) {
            int w = sceIoWrite(f, buf.data() + pos, used - pos);
            if (w <= 0)
                ok = false;
            else
                pos += w;
        }
        used = 0;
        return ok;
    }

    bool put(const uint8_t* d, size_t n)
    {
        if (f < 0)
            return false;
        if (used + n > buf.size() && !flush())
            return false;
        if (n >= buf.size()) {
            size_t pos = 0;
            while (pos < n) {
                int w = sceIoWrite(f, d + pos, n - pos);
                if (w <= 0)
                    return ok = false;
                pos += w;
            }
            return true;
        }
        memcpy(buf.data() + used, d, n);
        used += n;
        return true;
    }

    bool close()
    {
        if (f < 0)
            return ok;
        flush();
        sceIoClose(f);
        f = -1;
        return ok;
    }
};

bool fetch(const std::string& url, const std::string& path, Progress* p, std::string& err, Session* s)
{
    std::string part = path + ".part";
    fs::mkdirs(fs::parent(path));
    std::string target = url;
    Writer out(s ? 128 * 1024 : 1024 * 1024);
    for (int attempt = 0; attempt < 3; attempt++) {
        int64_t have = fs::size(part);
        Ctx ctx;
        ctx.progress = p;
        ctx.offset = have > 0 ? (uint64_t)have : 0;
        if (!out.open(part, ctx.offset != 0)) {
            err = "Не удалось создать " + part;
            return false;
        }
        CURL* h = nullptr;
        bool checked = false;
        ctx.sink = [&](const uint8_t* d, size_t n) {
            if (!checked) {
                checked = true;
                long code = 0;
                curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
                if (ctx.offset && code == 200) {
                    ctx.offset = 0;
                    if (!out.open(part, false))
                        return false;
                }
            }
            return out.put(d, n);
        };
        h = make(target, ctx, s);
        if (ctx.offset)
            curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)ctx.offset);
        CURLcode rc = curl_easy_perform(h);
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        bool html = is_html(h);
        bool wrote = out.close();
        if (rc == CURLE_OK && !wrote)
            rc = CURLE_WRITE_ERROR;
        if (rc != CURLE_OK) {
            if (code == 416 && ctx.offset) {
                release(h, s);
                sceIoRemove(part.c_str());
                continue;
            }
            err = ctx.failed || !wrote ? "Ошибка записи на карту" : curl_err(h, rc);
            release(h, s);
            return false;
        }
        release(h, s);
        if (html && is_drive(target)) {
            std::string page;
            fs::read(part, page);
            sceIoRemove(part.c_str());
            std::string next = drive_confirm(page);
            if (next.empty()) {
                err = "Google Drive не отдал файл, возможно лимит скачиваний";
                return false;
            }
            target = next;
            continue;
        }
        sceIoRemove(path.c_str());
        if (sceIoRename(part.c_str(), path.c_str()) < 0) {
            err = "Не удалось переименовать файл";
            return false;
        }
        return true;
    }
    err = "Не удалось скачать файл";
    return false;
}

bool stream(const std::string& url, const Sink& sink, Progress* p, std::string& err)
{
    std::string target = url;
    for (int attempt = 0; attempt < 2; attempt++) {
        Ctx ctx;
        ctx.progress = p;
        CURL* h = nullptr;
        bool checked = false;
        bool html = false;
        std::string page;
        const size_t CHUNK = 512 * 1024;
        std::vector<uint8_t> pending;
        pending.reserve(CHUNK + 64 * 1024);
        ctx.sink = [&](const uint8_t* d, size_t n) {
            if (!checked) {
                checked = true;
                html = is_html(h) && is_drive(target);
            }
            if (html) {
                page.append((const char*)d, n);
                return page.size() < 1024 * 1024;
            }
            if (pending.empty() && n >= CHUNK)
                return sink(d, n);
            pending.insert(pending.end(), d, d + n);
            if (pending.size() < CHUNK)
                return true;
            bool r = sink(pending.data(), pending.size());
            pending.clear();
            return r;
        };
        h = make(target, ctx);
        CURLcode rc = curl_easy_perform(h);
        if (rc == CURLE_OK && !html && !pending.empty()) {
            if (!sink(pending.data(), pending.size())) {
                ctx.failed = true;
                rc = CURLE_WRITE_ERROR;
            }
            pending.clear();
        }
        if (rc != CURLE_OK) {
            err = ctx.failed && !html ? "Ошибка обработки данных" : curl_err(h, rc);
            curl_easy_cleanup(h);
            return false;
        }
        curl_easy_cleanup(h);
        if (html) {
            std::string next = drive_confirm(page);
            if (next.empty()) {
                err = "Google Drive не отдал файл, возможно лимит скачиваний";
                return false;
            }
            target = next;
            continue;
        }
        return true;
    }
    err = "Google Drive не отдал файл";
    return false;
}

}
