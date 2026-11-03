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

extern "C" int __wrap_fcntl(int, int cmd, ...)
{
    if (cmd == F_SETFD)
        return 0;
    errno = ENOSYS;
    return -1;
}

namespace net {

static uint8_t netmem[1024 * 1024];

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
}

void fini()
{
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

static CURL* make(const std::string& url, Ctx& ctx)
{
    CURL* h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_USERAGENT, "VPKGJ/1.0 (PS Vita)");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 512L * 1024);
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

bool get(const std::string& url, std::string& out, std::string& err)
{
    std::string target = url;
    for (int attempt = 0; attempt < 2; attempt++) {
        Ctx ctx;
        out.clear();
        ctx.sink = [&](const uint8_t* d, size_t n) {
            out.append((const char*)d, n);
            return out.size() < 64u * 1024 * 1024;
        };
        CURL* h = make(target, ctx);
        CURLcode rc = curl_easy_perform(h);
        bool html = is_html(h);
        if (rc != CURLE_OK) {
            err = curl_err(h, rc);
            curl_easy_cleanup(h);
            return false;
        }
        curl_easy_cleanup(h);
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

bool fetch(const std::string& url, const std::string& path, Progress* p, std::string& err)
{
    std::string part = path + ".part";
    fs::mkdirs(fs::parent(path));
    std::string target = url;
    for (int attempt = 0; attempt < 3; attempt++) {
        int64_t have = fs::size(part);
        Ctx ctx;
        ctx.progress = p;
        ctx.offset = have > 0 ? (uint64_t)have : 0;
        SceUID f = sceIoOpen(part.c_str(), SCE_O_WRONLY | SCE_O_CREAT | (ctx.offset ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
        if (f < 0) {
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
                    sceIoClose(f);
                    f = sceIoOpen(part.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
                    ctx.offset = 0;
                    if (f < 0)
                        return false;
                }
            }
            size_t pos = 0;
            while (pos < n) {
                int w = sceIoWrite(f, d + pos, n - pos);
                if (w <= 0)
                    return false;
                pos += w;
            }
            return true;
        };
        h = make(target, ctx);
        if (ctx.offset)
            curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)ctx.offset);
        CURLcode rc = curl_easy_perform(h);
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        bool html = is_html(h);
        if (f >= 0)
            sceIoClose(f);
        if (rc != CURLE_OK) {
            if (code == 416 && ctx.offset) {
                curl_easy_cleanup(h);
                sceIoRemove(part.c_str());
                continue;
            }
            err = ctx.failed ? "Ошибка записи на карту" : curl_err(h, rc);
            curl_easy_cleanup(h);
            return false;
        }
        curl_easy_cleanup(h);
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
        ctx.sink = [&](const uint8_t* d, size_t n) {
            if (!checked) {
                checked = true;
                html = is_html(h) && is_drive(target);
            }
            if (html) {
                page.append((const char*)d, n);
                return page.size() < 1024 * 1024;
            }
            return sink(d, n);
        };
        h = make(target, ctx);
        CURLcode rc = curl_easy_perform(h);
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
