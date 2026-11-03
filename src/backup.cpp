#include "backup.hpp"

#include "util.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/rtc.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

#define BACKUP_DIR (APP_DIR + "/backups")

uint64_t tree_size(const std::string& path)
{
    if (!fs::is_dir(path)) {
        int64_t s = fs::size(path);
        return s > 0 ? s : 0;
    }
    uint64_t total = 0;
    for (auto& n : fs::list(path))
        total += tree_size(path + "/" + n);
    return total;
}

static bool copy_file(const std::string& from, const std::string& to)
{
    SceUID in = sceIoOpen(from.c_str(), SCE_O_RDONLY, 0);
    if (in < 0)
        return false;
    fs::mkdirs(fs::parent(to));
    SceUID out = sceIoOpen(to.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) {
        sceIoClose(in);
        return false;
    }
    static std::vector<uint8_t> buf(256 * 1024);
    bool ok = true;
    for (;;) {
        int r = sceIoRead(in, buf.data(), buf.size());
        if (r < 0)
            ok = false;
        if (r <= 0)
            break;
        int pos = 0;
        while (pos < r) {
            int w = sceIoWrite(out, buf.data() + pos, r - pos);
            if (w <= 0) {
                ok = false;
                break;
            }
            pos += w;
        }
        if (!ok)
            break;
    }
    sceIoClose(in);
    sceIoClose(out);
    return ok;
}

static bool copy_tree(const std::string& from, const std::string& to)
{
    if (!fs::is_dir(from))
        return copy_file(from, to);
    fs::mkdirs(to);
    for (auto& n : fs::list(from))
        if (!copy_tree(from + "/" + n, to + "/" + n))
            return false;
    return true;
}

static std::string stamp()
{
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    char b[32];
    snprintf(b, sizeof(b), "%04d%02d%02d-%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return b;
}

bool backup_create(const std::string& label, const std::vector<std::string>& paths, std::string& err)
{
    std::vector<std::string> present;
    for (auto& p : paths)
        if (fs::exists(p))
            present.push_back(p);
    if (present.empty())
        return true;
    std::string dir = BACKUP_DIR + "/" + stamp();
    for (int i = 2; fs::exists(dir); i++)
        dir = BACKUP_DIR + "/" + stamp() + "_" + std::to_string(i);
    fs::mkdirs(dir);
    std::string manifest;
    int n = 0;
    for (auto& p : present) {
        std::string rel = "f" + std::to_string(n++);
        if (!copy_tree(p, dir + "/" + rel)) {
            fs::rmtree(dir);
            err = "Не удалось сделать копию " + p;
            return false;
        }
        manifest += p + "\t" + rel + "\n";
    }
    fs::write(dir + "/manifest.txt", manifest);
    fs::write(dir + "/label.txt", label);
    log_line("backup %s: %s", dir.c_str(), label.c_str());
    return true;
}

std::vector<Backup> backup_list()
{
    std::vector<Backup> out;
    auto names = fs::list(BACKUP_DIR);
    std::sort(names.rbegin(), names.rend());
    for (auto& n : names) {
        Backup b;
        b.dir = BACKUP_DIR + "/" + n;
        std::string manifest;
        if (!fs::read(b.dir + "/manifest.txt", manifest))
            continue;
        fs::read(b.dir + "/label.txt", b.label);
        if (n.size() >= 15)
            b.date = n.substr(6, 2) + "." + n.substr(4, 2) + "." + n.substr(0, 4) + " " + n.substr(9, 2) + ":" + n.substr(11, 2);
        std::istringstream in(manifest);
        std::string line;
        while (std::getline(in, line)) {
            size_t t = line.find('\t');
            if (t != std::string::npos)
                b.items.push_back({line.substr(0, t), line.substr(t + 1)});
        }
        b.size = tree_size(b.dir);
        out.push_back(b);
    }
    return out;
}

bool backup_restore(const Backup& b, std::string& err)
{
    for (auto& it : b.items) {
        std::string src = b.dir + "/" + it.second;
        if (fs::is_dir(src))
            fs::rmtree(it.first);
        if (!copy_tree(src, it.first)) {
            err = "Не удалось восстановить " + it.first;
            return false;
        }
    }
    return true;
}

bool backup_delete(const Backup& b)
{
    return fs::rmtree(b.dir);
}

std::string savedata_path(const std::string& titleid)
{
    return "ux0:user/00/savedata/" + titleid;
}
