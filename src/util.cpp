#include "util.hpp"

#include <psp2/io/dirent.h>
#include <psp2/io/devctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

const std::string APP_DIR = "ux0:data/VPKGJ";


namespace fs {

bool exists(const std::string& path)
{
    SceIoStat st;
    return sceIoGetstat(path.c_str(), &st) >= 0;
}

bool is_dir(const std::string& path)
{
    SceIoStat st;
    return sceIoGetstat(path.c_str(), &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

bool mkdirs(const std::string& path)
{
    size_t start = path.find(':');
    start = start == std::string::npos ? 0 : start + 1;
    for (size_t i = start; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            std::string part = path.substr(0, i);
            if (part.size() > start && !part.empty() && part.back() != ':')
                sceIoMkdir(part.c_str(), 0777);
        }
    }
    return is_dir(path);
}

bool rmtree(const std::string& path)
{
    SceUID d = sceIoDopen(path.c_str());
    if (d < 0)
        return sceIoRemove(path.c_str()) >= 0;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, ".."))
            continue;
        std::string sub = path + "/" + e.d_name;
        if (SCE_S_ISDIR(e.d_stat.st_mode))
            rmtree(sub);
        else
            sceIoRemove(sub.c_str());
    }
    sceIoDclose(d);
    return sceIoRmdir(path.c_str()) >= 0;
}

bool read(const std::string& path, std::string& out)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0)
        return false;
    int64_t len = sceIoLseek(f, 0, SCE_SEEK_END);
    sceIoLseek(f, 0, SCE_SEEK_SET);
    out.resize(len > 0 ? (size_t)len : 0);
    size_t pos = 0;
    while (pos < out.size()) {
        int r = sceIoRead(f, &out[pos], out.size() - pos);
        if (r <= 0)
            break;
        pos += r;
    }
    sceIoClose(f);
    out.resize(pos);
    return true;
}

bool write(const std::string& path, const void* data, size_t size)
{
    mkdirs(parent(path));
    std::string tmp = path + ".tmp";
    SceUID f = sceIoOpen(tmp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (f < 0)
        return false;
    const uint8_t* p = (const uint8_t*)data;
    size_t pos = 0;
    while (pos < size) {
        int w = sceIoWrite(f, p + pos, size - pos);
        if (w <= 0) {
            sceIoClose(f);
            sceIoRemove(tmp.c_str());
            return false;
        }
        pos += w;
    }
    sceIoClose(f);
    sceIoRemove(path.c_str());
    return sceIoRename(tmp.c_str(), path.c_str()) >= 0;
}

bool write(const std::string& path, const std::string& data)
{
    return write(path, data.data(), data.size());
}

int64_t size(const std::string& path)
{
    SceIoStat st;
    if (sceIoGetstat(path.c_str(), &st) < 0)
        return -1;
    return st.st_size;
}

uint64_t free_space(const std::string& dev)
{
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    std::string d = dev.empty() ? "ux0:" : dev;
    if (sceIoDevctl(d.c_str(), 0x3001, nullptr, 0, &info, sizeof(info)) < 0)
        return 0;
    return info.free_size;
}

std::vector<std::string> list(const std::string& path)
{
    std::vector<std::string> out;
    SceUID d = sceIoDopen(path.c_str());
    if (d < 0)
        return out;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") && strcmp(e.d_name, ".."))
            out.push_back(e.d_name);
    }
    sceIoDclose(d);
    return out;
}

std::string parent(const std::string& path)
{
    size_t p = path.rfind('/');
    if (p == std::string::npos)
        return path.substr(0, path.find(':') + 1);
    return path.substr(0, p);
}

std::string device(const std::string& path)
{
    size_t p = path.find(':');
    return p == std::string::npos ? "ux0:" : path.substr(0, p + 1);
}

}

std::string fmt_size(uint64_t b)
{
    char buf[32];
    if (b >= 1024ull * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f ГБ", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f МБ", b / (1024.0 * 1024));
    else if (b >= 1024)
        snprintf(buf, sizeof(buf), "%llu КБ", (unsigned long long)(b / 1024));
    else
        snprintf(buf, sizeof(buf), "%llu Б", (unsigned long long)b);
    std::string s = buf;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static uint32_t next_cp(const std::string& s, size_t& i)
{
    unsigned char c = s[i];
    if (c < 0x80) {
        i += 1;
        return c;
    }
    if ((c >> 5) == 6 && i + 1 < s.size()) {
        uint32_t cp = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f);
        i += 2;
        return cp;
    }
    if ((c >> 4) == 14 && i + 2 < s.size()) {
        uint32_t cp = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
        i += 3;
        return cp;
    }
    i += 1;
    return c;
}

static void put_cp(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xc0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3f));
    } else {
        out += (char)(0xe0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3f));
        out += (char)(0x80 | (cp & 0x3f));
    }
}

std::string lower(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = next_cp(s, i);
        if (cp >= 'A' && cp <= 'Z')
            cp += 32;
        else if (cp >= 0x410 && cp <= 0x42f)
            cp += 32;
        else if (cp == 0x401)
            cp = 0x451;
        if (cp == 0x451)
            cp = 0x435;
        put_cp(out, cp);
    }
    return out;
}

bool starts_with(const std::string& s, const std::string& p)
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p)
{
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string hash_name(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

uint64_t now_ms()
{
    return sceKernelGetProcessTimeWide() / 1000;
}

void log_line(const char* fmt, ...)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    n = std::min(n, (int)sizeof(buf) - 2);
    buf[n++] = '\n';
    std::string path = APP_DIR + "/log.txt";
    SceUID f = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (f >= 0) {
        sceIoWrite(f, buf, n);
        sceIoClose(f);
    }
}
