#include "tasks.hpp"

#include "backup.hpp"
#include "images.hpp"
#include "pkg.hpp"
#include "promote.hpp"
#include "shaccg.hpp"
#include "util.hpp"
#include "zip.hpp"

#include <psp2/io/fcntl.h>
#include <cstring>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include <mbedtls/sha256.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

static const std::string TEMP = "ux0:temp/vpkgj";

std::string Task::get_stage()
{
    std::lock_guard<Mutex> lock(m);
    return stage;
}

std::string Task::get_note()
{
    std::lock_guard<Mutex> lock(m);
    return note;
}

std::string Task::get_error()
{
    std::lock_guard<Mutex> lock(m);
    return error;
}

void Task::set_stage(const std::string& s)
{
    std::lock_guard<Mutex> lock(m);
    stage = s;
}

std::string tai_dir()
{
    return fs::exists("ux0:tai/config.txt") ? "ux0:tai" : "ur0:tai";
}

static std::vector<std::string> links(const std::string& primary, const std::string& mirror)
{
    std::vector<std::string> out;
    for (const std::string& s : {primary, mirror}) {
        std::string cur;
        for (char c : s + "|") {
            if (c == '|' || c == ' ') {
                if (!trim(cur).empty())
                    out.push_back(trim(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    return out;
}

std::string bg_file(const std::string& url)
{
    return APP_DIR + "/bg/" + hash_name(url);
}

std::string first_link(const std::string& primary)
{
    auto l = links(primary, "");
    return l.empty() ? "" : l[0];
}

static bool download(Task& t, const std::vector<std::string>& urls, const std::string& path, std::string& err)
{
    if (urls.empty()) {
        err = "Нет ссылки на файл";
        return false;
    }
    std::string ready = bg_file(urls[0]);
    if (fs::exists(ready)) {
        t.set_stage("Скачано в фоне");
        sceIoRemove(path.c_str());
        if (sceIoRename(ready.c_str(), path.c_str()) >= 0)
            return true;
    }
    for (size_t i = 0; i < urls.size(); i++) {
        t.set_stage(i ? "Загрузка с зеркала" : "Загрузка");
        t.progress.done = 0;
        t.progress.total = 0;
        if (net::fetch(urls[i], path, &t.progress, err))
            return true;
        log_line("download %s: %s", urls[i].c_str(), err.c_str());
        if (t.progress.cancel)
            return false;
        sceIoRemove((path + ".part").c_str());
    }
    return false;
}

static std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + sep) {
        if (c == sep) {
            if (!trim(cur).empty())
                out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

static const char* FIX_SOON = ". Мы уже знаем, скоро поправим";

static bool sha_file(const std::string& path, std::string& out, net::Progress* p)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0)
        return false;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    std::vector<uint8_t> buf(512 * 1024);
    uint64_t done = 0;
    if (p) {
        p->total = fs::size(path);
        p->done = 0;
    }
    int r;
    while ((r = sceIoRead(f, buf.data(), buf.size())) > 0) {
        mbedtls_sha256_update(&c, buf.data(), r);
        done += r;
        if (p)
            p->done = done;
        if (p && p->cancel)
            break;
    }
    sceIoClose(f);
    unsigned char d[32];
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
    static const char* hx = "0123456789abcdef";
    out.clear();
    for (unsigned char b : d) {
        out += hx[b >> 4];
        out += hx[b & 15];
    }
    return r >= 0 && !(p && p->cancel);
}

static bool is_zip(const std::string& path);

static bool fetch_checked(Task& t, const std::vector<std::string>& urls, const std::string& path, const std::string& sha,
                          const char* what, std::string& err)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!download(t, urls, path, err))
            return false;
        if (sha.size() == 64) {
            t.set_stage("Проверка хеша");
            std::string h;
            if (!sha_file(path, h, &t.progress)) {
                err = t.progress.cancel ? "Отменено" : "Не удалось прочитать скачанный файл";
                return false;
            }
            if (lower(sha) != h) {
                log_line("sha mismatch %s: %s", what, h.c_str());
                err = std::string("Не прошла проверка: ") + what + " скачался битым, хеш не совпал" + FIX_SOON;
                sceIoRemove(path.c_str());
                continue;
            }
        }
        return true;
    }
    return false;
}

static std::string norm_dest(const std::string& d)
{
    std::string r = trim(d).empty() ? "ux0:data" : trim(d);
    while (r.size() > 4 && r.back() == '/')
        r.pop_back();
    return r;
}

static std::vector<std::pair<std::string, std::string>> data_parts(const Entry& e)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& part : split(e.data, ';')) {
        std::string src = trim(part), dest = e.data_path;
        if (src.empty())
            continue;
        size_t gt = src.find('>');
        if (gt != std::string::npos) {
            dest = trim(src.substr(gt + 1));
            src = trim(src.substr(0, gt));
        }
        out.push_back({src, norm_dest(dest)});
    }
    return out;
}

static std::vector<std::string> top_paths(const std::vector<std::string>& names, const std::string& dest)
{
    std::set<std::string> tops;
    for (auto n : names) {
        for (auto& c : n)
            if (c == '\\')
                c = '/';
        if (n.empty() || n[0] == '/' || n.find("../") != std::string::npos)
            continue;
        size_t s = n.find('/');
        std::string top = s == std::string::npos ? n : n.substr(0, s);
        if (!top.empty() && top != "." && top != "__MACOSX")
            tops.insert(dest + "/" + top);
    }
    return std::vector<std::string>(tops.begin(), tops.end());
}

static Mutex data_m;

static std::string data_key(const Entry& e)
{
    return std::string(category_key(e.cat)) + "/" + e.id;
}

static std::string data_fp(const Entry& e)
{
    return hash_name(e.data + "|" + e.data_path);
}

static std::map<std::string, std::pair<std::string, std::vector<std::string>>> data_load()
{
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> m;
    std::string s;
    if (!fs::read(APP_DIR + "/datastate.txt", s))
        return m;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f = split(line, '\t');
        if (f.size() >= 2)
            m[f[0]] = {f[1], f.size() > 2 ? split(f[2], '|') : std::vector<std::string>()};
    }
    return m;
}

static void data_record(const Entry& e, const std::vector<std::string>& paths)
{
    std::lock_guard<Mutex> lock(data_m);
    auto m = data_load();
    m[data_key(e)] = {data_fp(e), paths};
    std::string s;
    for (auto& kv : m) {
        std::string ps;
        for (auto& p : kv.second.second)
            ps += (ps.empty() ? "" : "|") + p;
        s += kv.first + "\t" + kv.second.first + "\t" + ps + "\n";
    }
    fs::write(APP_DIR + "/datastate.txt", s);
}

bool data_installed(const Entry& e, bool probe)
{
    if (e.data.empty())
        return true;
    {
        std::lock_guard<Mutex> lock(data_m);
        auto m = data_load();
        auto it = m.find(data_key(e));
        if (it != m.end()) {
            if (it->second.first != data_fp(e) || it->second.second.empty())
                return false;
            for (auto& p : it->second.second)
                if (!fs::exists(p))
                    return false;
            return true;
        }
    }
    if (!probe)
        return false;
    std::vector<std::string> paths;
    for (auto& part : data_parts(e)) {
        if (lower(part.second) != "ux0:data") {
            if (!fs::exists(part.second) || fs::list(part.second).empty())
                return false;
            paths.push_back(part.second);
            continue;
        }
        std::vector<std::string> names;
        std::string err;
        std::string url = first_link(part.first);
        if (url.empty() || !zip_remote_names(url, names, err))
            return false;
        std::vector<std::string> tops = top_paths(names, part.second);
        if (tops.empty())
            return false;
        for (auto& p : tops)
            if (!fs::exists(p))
                return false;
        paths.insert(paths.end(), tops.begin(), tops.end());
    }
    if (paths.empty())
        return false;
    data_record(e, paths);
    log_line("data %s already present, recorded", e.id.c_str());
    return true;
}

static bool preflight(Task& t, const std::vector<std::pair<std::string, const char*>>& files, std::string& err)
{
    t.set_stage("Проверка файлов на сервере");
    for (auto& f : files) {
        std::string url = first_link(f.first);
        if (url.empty()) {
            err = std::string("Нет ссылки: ") + f.second + FIX_SOON;
            return false;
        }
        std::vector<std::string> names;
        std::string e;
        if (zip_remote_names(url, names, e))
            continue;
        log_line("preflight %s %s: %s", f.second, url.c_str(), e.c_str());
        if (starts_with(e, "http 4")) {
            err = std::string(f.second) + " сейчас недоступен на сервере (" + e.substr(5) + ")" + FIX_SOON;
            return false;
        }
        if (e == "Не zip") {
            err = std::string(f.second) + " на сервере битый, это не zip" + FIX_SOON;
            return false;
        }
    }
    return true;
}

static bool is_zip(const std::string& path)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0)
        return false;
    char b[264] = {0};
    int r = sceIoRead(f, b, sizeof(b));
    sceIoClose(f);
    if (r >= 2 && b[0] == 'P' && b[1] == 'K')
        return true;
    return r >= 262 && memcmp(b + 257, "ustar", 5) == 0;
}

static bool tai_add(const std::string& section, const std::string& plugin, std::string& err)
{
    std::string dir = tai_dir();
    std::string cfg = dir + "/config.txt";
    std::string text;
    if (!fs::read(cfg, text)) {
        err = "Не найден " + cfg;
        return false;
    }
    if (!fs::exists(cfg + ".vpkgj.bak"))
        fs::write(cfg + ".vpkgj.bak", text);
    std::string sec = section.empty() ? "*ALL" : section[0] == '*' ? section : "*" + section;
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    int at = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        if (trim(lines[i]) == sec) {
            at = (int)i;
            break;
        }
    }
    if (at < 0) {
        if (!lines.empty() && !trim(lines.back()).empty())
            lines.push_back("");
        lines.push_back(sec);
        lines.push_back(plugin);
    } else {
        for (size_t i = at + 1; i < lines.size(); i++) {
            std::string l = trim(lines[i]);
            if (!l.empty() && l[0] == '*')
                break;
            if (lower(l) == lower(plugin))
                return true;
        }
        lines.insert(lines.begin() + at + 1, plugin);
    }
    std::string out;
    for (auto& l : lines)
        out += l + "\n";
    if (!fs::write(cfg, out)) {
        err = "Не удалось записать " + cfg;
        return false;
    }
    return true;
}

bool tai_add_plugin(const std::string& section, const std::string& path, std::string& err)
{
    return tai_add(section, path, err);
}

bool tai_remove_plugin(const std::string& path)
{
    bool changed = false;
    for (const char* cfg : {"ux0:tai/config.txt", "ur0:tai/config.txt"}) {
        std::string text;
        if (!fs::read(cfg, text) || lower(text).find(lower(path)) == std::string::npos)
            continue;
        std::istringstream in(text);
        std::string line, out;
        while (std::getline(in, line)) {
            std::string l = line;
            if (!l.empty() && l.back() == '\r')
                l.pop_back();
            if (lower(trim(l)) == lower(path))
                continue;
            out += l + "\n";
        }
        fs::write(cfg, out);
        changed = true;
    }
    return changed;
}

static std::string base_name(const std::string& p)
{
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

static bool install_plugin_files(Task& t, const Entry& pe, const std::string& file, const std::string& section, std::string& err)
{
    std::string dir = tai_dir();
    std::vector<std::string> placed;
    if (is_zip(file)) {
        t.set_stage("Распаковка");
        bool ok = zip_extract(file, dir, &t.progress, err, [&](const std::string& n) {
            std::string l = lower(n);
            if (!ends_with(l, ".suprx") && !ends_with(l, ".skprx"))
                return std::string();
            std::string out = dir + "/" + base_name(n);
            placed.push_back(out);
            return out;
        });
        if (!ok)
            return false;
    } else if (pe.id == "vpkgj_daemon") {
        std::string out = APP_DIR + "/vpkgj_daemon.suprx";
        fs::mkdirs(APP_DIR);
        sceIoRemove(out.c_str());
        if (sceIoRename(file.c_str(), out.c_str()) < 0) {
            std::string data;
            if (!fs::read(file, data) || !fs::write(out, data)) {
                err = "Не удалось скопировать демон";
                return false;
            }
        }
        sceIoRemove((APP_DIR + "/daemon_off").c_str());
        placed.push_back(out);
    } else {
        std::string name = base_name(split(pe.url, '|').empty() ? pe.url : split(pe.url, '|')[0]);
        size_t q = name.find('?');
        if (q != std::string::npos)
            name = name.substr(0, q);
        std::string l = lower(name);
        if (!ends_with(l, ".suprx") && !ends_with(l, ".skprx"))
            name = pe.id + ".suprx";
        std::string out = dir + "/" + name;
        fs::mkdirs(dir);
        sceIoRemove(out.c_str());
        if (sceIoRename(file.c_str(), out.c_str()) < 0) {
            std::string data;
            if (!fs::read(file, data) || !fs::write(out, data)) {
                err = "Не удалось скопировать плагин";
                return false;
            }
        }
        placed.push_back(out);
    }
    if (placed.empty()) {
        err = "В архиве нет плагинов";
        return false;
    }
    for (auto& p : placed) {
        std::string sec = section;
        if (ends_with(lower(p), ".skprx"))
            sec = "*KERNEL";
        if (!tai_add(sec, p, err))
            return false;
    }
    std::lock_guard<Mutex> lock(t.m);
    t.note = "Перезагрузи консоль, чтобы плагин включился";
    t.reboot = true;
    return true;
}

bool plugin_present(const Entry& p)
{
    std::string cfg;
    for (const char* path : {"ux0:tai/config.txt", "ur0:tai/config.txt"}) {
        std::string s;
        if (fs::read(path, s))
            cfg += lower(s) + "\n";
    }
    std::vector<std::string> keys = split(lower(p.check), '|');
    if (keys.empty()) {
        std::string name = base_name(split(p.url, '|').empty() ? p.url : split(p.url, '|')[0]);
        size_t q = name.find('?');
        keys.push_back(lower(q == std::string::npos ? name : name.substr(0, q)));
    }
    bool files_only = !keys.empty();
    for (auto& k : keys) {
        if (starts_with(k, "file:")) {
            if (fs::exists(k.substr(5)) && !fs::is_dir(k.substr(5)))
                return true;
        } else {
            files_only = false;
        }
    }
    if (files_only)
        return false;
    std::istringstream in(cfg);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '*')
            continue;
        for (auto& k : keys)
            if (!k.empty() && line.find(k) != std::string::npos)
                return true;
    }
    return false;
}

static bool install_deps(Task& t, std::string& err)
{
    for (auto& d : t.deps) {
        if (plugin_present(d))
            continue;
        if (d.url.empty()) {
            std::lock_guard<Mutex> lock(t.m);
            t.note = "Не хватает " + d.name + ". " + d.description;
            continue;
        }
        t.set_stage("Ставлю " + d.name);
        std::string f = APP_DIR + "/dl/dep_" + hash_name(d.id + d.url);
        bool ok = download(t, links(d.url, d.mirror), f, err) && install_plugin_files(t, d, f, d.tai_section, err);
        sceIoRemove(f.c_str());
        if (!ok) {
            err = d.name + ": " + err;
            return false;
        }
        std::lock_guard<Mutex> lock(t.m);
        t.note = "Перезагрузи консоль, чтобы плагины включились";
        t.reboot = true;
    }
    return true;
}

static bool install_vpk(Task& t, const std::string& file, std::string& err)
{
    std::string dir = TEMP + "/app";
    fs::rmtree(dir);
    fs::mkdirs(dir);
    t.set_stage("Распаковка");
    if (!zip_extract(file, dir, &t.progress, err))
        return false;
    t.set_stage("Установка");
    bool ok = promote_app(dir, err);
    fs::rmtree(dir);
    return ok;
}

static bool install_data(Task& t, const std::string& file, const std::string& dest, std::string& err)
{
    if (!is_zip(file)) {
        err = "Data должна быть zip архивом";
        return false;
    }
    std::string d = dest.empty() ? "ux0:data" : dest;
    while (d.size() > 4 && d.back() == '/')
        d.pop_back();
    fs::mkdirs(d);
    t.set_stage("Распаковка data");
    return zip_extract(file, d, &t.progress, err);
}

static bool enough_space(const Entry& e, double factor, std::string& err)
{
    if (!e.size)
        return true;
    uint64_t need = (uint64_t)(e.size * factor);
    uint64_t have = fs::free_space("ux0:");
    if (have && have < need) {
        err = "Мало места: нужно " + fmt_size(need) + ", свободно " + fmt_size(have);
        return false;
    }
    return true;
}

static bool install_translation(Task& t, const std::string& file, std::string& err)
{
    const Entry& e = t.entry;
    std::string type = e.install.empty() ? "repatch" : lower(e.install);
    if (type == "vpk")
        return install_vpk(t, file, err);
    if (type == "data")
        return install_data(t, file, e.data_path, err);
    if (type == "plugin")
        return install_plugin_files(t, e, file, e.id, err);
    if (!is_zip(file)) {
        err = "Перевод должен быть zip архивом";
        return false;
    }
    std::vector<std::string> names;
    if (!zip_list(file, names, err))
        return false;
    bool has_root = false, has_tid = false;
    for (auto& n : names) {
        std::string l = lower(n);
        if (starts_with(l, "repatch/"))
            has_root = true;
        if (starts_with(n, e.id + "/"))
            has_tid = true;
    }
    bool with_plugin = type == "repatch+plugin";
    std::string tai = tai_dir();
    std::vector<std::string> plugins;
    t.set_stage("Распаковка");
    bool ok = zip_extract(file, "ux0:", &t.progress, err, [&](const std::string& n) {
        std::string l = lower(n);
        if (ends_with(l, ".suprx") || ends_with(l, ".skprx")) {
            if (!with_plugin)
                return std::string();
            std::string out = tai + "/" + base_name(n);
            plugins.push_back(out);
            return out;
        }
        if (has_root) {
            if (!starts_with(l, "repatch/"))
                return std::string();
            return "ux0:rePatch/" + n.substr(8);
        }
        if (has_tid)
            return "ux0:rePatch/" + n;
        if (ends_with(l, ".txt") || ends_with(l, ".md"))
            return std::string();
        return "ux0:rePatch/" + e.id + "/" + n;
    });
    if (!ok)
        return false;
    for (auto& p : plugins)
        if (!tai_add(ends_with(lower(p), ".skprx") ? "*KERNEL" : "*" + e.id, p, err))
            return false;
    if (!plugins.empty()) {
        std::lock_guard<Mutex> lock(t.m);
        t.note = "Перезагрузи консоль, чтобы плагин включился";
    t.reboot = true;
    }
    return true;
}

static bool self_update(Task& t, const std::string& file, bool staged, std::string& err)
{
    t.set_stage(staged ? "Подготовка обновления" : "Обновление VPKGJ");
    std::string dir = staged ? APP_DIR + "/update" : "ux0:app/VPKG00001";
    if (staged) {
        fs::rmtree(dir);
        sceIoRemove((APP_DIR + "/update.ready").c_str());
        fs::mkdirs(dir);
    } else {
        images_close_pak();
    }
    bool ok = zip_extract(file, dir, &t.progress, err, [&](const std::string& n) {
        std::string l = lower(n);
        if (starts_with(l, "sce_sys/package/"))
            return std::string();
        return dir + "/" + n;
    });
    if (!ok)
        return false;
    if (staged && !fs::exists(dir + "/eboot.bin")) {
        err = "В обновлении нет eboot.bin";
        return false;
    }
    std::lock_guard<Mutex> lock(t.m);
    t.restart = true;
    t.staged = staged;
    t.note = staged ? "VPKGJ закроется, обновится и запустится сам" : "Перезапусти VPKGJ, чтобы включилась новая версия";
    return true;
}

static bool install_shaccg(Task& t, std::string& err)
{
    const Entry& e = t.entry;
    if (shaccg::state() == shaccg::SH_DIR) {
        t.set_stage("Убираю папку на месте libshacccg.suprx");
        fs::rmtree("ur0:data/libshacccg.suprx");
        if (fs::exists("ur0:data/libshacccg.suprx")) {
            err = "Не удалось удалить папку ur0:data/libshacccg.suprx";
            return false;
        }
    }
    if (shaccg::state() == shaccg::SH_OK)
        return true;
    if (!shaccg::shark_installed()) {
        std::string f = APP_DIR + "/dl/shark.vpk";
        t.set_stage("Скачиваю ShaRKF00D");
        bool ok = download(t, links(e.url, e.mirror), f, err) && install_vpk(t, f, err);
        sceIoRemove(f.c_str());
        if (!ok) {
            err = "ShaRKF00D: " + err;
            return false;
        }
    }
    if (!shaccg::psm_installed()) {
        const char* urls[2] = {
            "https://archive.org/download/psm-runtime/IP9100-PCSI00011_00-PSMRUNTIME000000.pkg",
            "https://archive.org/download/psm-runtime/IP9100-PCSI00011_00-PSMRUNTIME000000-A0201-V0100-e4708b1c1c71116c29632c23df590f68edbfc341-PE.pkg",
        };
        const char* dst[2] = {"ux0:data/Runtime1.00.pkg", "ux0:data/Runtime2.01.pkg"};
        for (int i = 0; i < 2; i++) {
            t.set_stage(std::string("Скачиваю PSM Runtime ") + (i ? "2.01" : "1.00"));
            if (!download(t, {urls[i]}, dst[i], err)) {
                err = "PSM Runtime: " + err;
                return false;
            }
        }
    }
    std::lock_guard<Mutex> lock(t.m);
    t.shaccg = true;
    return true;
}

void Tasks::run(Task& t)
{
    const Entry& e = t.entry;
    std::string err;
    std::string dl = APP_DIR + "/dl/" + std::string(category_key(e.cat)) + "_" + hash_name(e.id + e.url);
    if (e.install == "self" || e.install == "self_daemon") {
        bool ok = download(t, links(e.url, e.mirror), dl, err) && self_update(t, dl, e.install == "self_daemon", err);
        sceIoRemove(dl.c_str());
        if (ok) {
            t.set_stage("Готово");
            t.state = TS_DONE;
        } else {
            std::lock_guard<Mutex> lock(t.m);
            t.error = t.progress.cancel ? "Отменено" : err;
            t.state = t.progress.cancel ? TS_CANCELED : TS_FAILED;
        }
        return;
    }
    if (e.install == "shaccg") {
        bool ok = install_shaccg(t, err);
        std::lock_guard<Mutex> lock(t.m);
        if (ok) {
            t.stage = "Готово";
            t.state = TS_DONE;
        } else {
            t.error = t.progress.cancel ? "Отменено" : err;
            t.state = t.progress.cancel ? TS_CANCELED : TS_FAILED;
            log_line("shaccg failed: %s", t.error.c_str());
        }
        return;
    }
    bool ok = false;
    bool deps_missing = false;
    for (auto& d : t.deps)
        if (!plugin_present(d))
            deps_missing = true;
    std::string berr;
    if (e.cat == CAT_PLUGINS || deps_missing || (e.cat == CAT_TRANSLATIONS && lower(e.install).find("plugin") != std::string::npos)) {
        t.set_stage("Резервная копия config.txt");
        if (!backup_create("Перед установкой " + e.name + ": config.txt", {"ux0:tai/config.txt", "ur0:tai/config.txt"}, berr))
            log_line("%s", berr.c_str());
    }
    if (e.cat == CAT_TRANSLATIONS) {
        std::string rp = "ux0:rePatch/" + e.id;
        if (fs::exists(rp) && tree_size(rp) < 300ull * 1024 * 1024) {
            t.set_stage("Резервная копия rePatch");
            if (!backup_create("Перед установкой " + e.name + ": rePatch/" + e.id, {rp}, berr))
                log_line("%s", berr.c_str());
        }
    }
    if (!install_deps(t, err)) {
    } else if (e.cat == CAT_GAMES) {
        if (enough_space(e, 1.05, err)) {
            std::vector<std::string> urls = links(e.url, e.mirror);
            for (size_t i = 0; i < urls.size() && !ok; i++) {
                std::string stage;
                t.set_stage("Загрузка и распаковка");
                ok = pkg_install(urls[i], e.zrif, e.sha256, TEMP + "/pkg", &t.progress, stage, err);
                if (t.progress.cancel)
                    break;
            }
            if (urls.empty())
                err = "Нет ссылки на PKG";
        }
    } else if (e.cat == CAT_PORTS || e.cat == CAT_HOMEBREW) {
        std::string was = installed->version_of(e);
        bool skip_data = false;
        if (!e.data.empty() && !was.empty() && was != (e.version.empty() ? "-" : e.version)) {
            t.set_stage("Проверка data");
            skip_data = data_installed(e, true);
        }
        auto parts = skip_data ? std::vector<std::pair<std::string, std::string>>() : data_parts(e);
        std::vector<std::pair<std::string, const char*>> check = {{e.url, "VPK"}};
        for (auto& p : parts)
            check.push_back({p.first, "Архив data"});
        if (enough_space(e, skip_data ? 1.2 : 2.1, err) && preflight(t, check, err) &&
            fetch_checked(t, links(e.url, e.data.empty() ? e.mirror : ""), dl, e.sha256, "VPK", err)) {
            ok = install_vpk(t, dl, err);
            sceIoRemove(dl.c_str());
            std::vector<std::string> recorded;
            for (size_t i = 0; ok && i < parts.size(); i++) {
                std::string dd = dl + "_data" + std::to_string(i);
                ok = fetch_checked(t, links(parts[i].first, i == 0 ? e.mirror : ""), dd, "", "архив data", err) &&
                     install_data(t, dd, parts[i].second, err);
                if (ok) {
                    std::vector<std::string> names;
                    std::string lerr;
                    if (zip_list(dd, names, lerr)) {
                        auto tops = top_paths(names, parts[i].second);
                        recorded.insert(recorded.end(), tops.begin(), tops.end());
                    }
                }
                sceIoRemove(dd.c_str());
            }
            if (ok && !parts.empty())
                data_record(e, recorded);
            if (ok && skip_data) {
                std::lock_guard<Mutex> lock(t.m);
                if (t.note.empty())
                    t.note = "Обновлён только VPK, data уже стоит";
            }
        }
    } else if (e.cat == CAT_TRANSLATIONS) {
        if (enough_space(e, 2.1, err)) {
            std::vector<std::string> parts;
            std::string cur;
            for (char c : e.url + ";") {
                if (c == ';') {
                    if (!trim(cur).empty())
                        parts.push_back(trim(cur));
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            ok = !parts.empty();
            if (!ok)
                err = "Нет ссылки на перевод";
            for (size_t i = 0; i < parts.size() && ok; i++) {
                std::string f = dl + "_" + std::to_string(i);
                ok = fetch_checked(t, links(parts[i], i == 0 ? e.mirror : ""), f, parts.size() == 1 ? e.sha256 : "", "архив перевода", err) &&
                     install_translation(t, f, err);
                sceIoRemove(f.c_str());
            }
        }
    } else if (e.cat == CAT_PLUGINS) {
        if (fetch_checked(t, links(e.url, e.mirror), dl, e.sha256, "плагин", err)) {
            ok = install_plugin_files(t, e, dl, e.tai_section, err);
            sceIoRemove(dl.c_str());
        }
    }
    if (ok) {
        installed->set(e);
        t.set_stage("Готово");
        t.state = TS_DONE;
    } else {
        std::lock_guard<Mutex> lock(t.m);
        t.error = t.progress.cancel ? "Отменено" : err;
        t.state = t.progress.cancel ? TS_CANCELED : TS_FAILED;
        log_line("task %s failed: %s", e.id.c_str(), t.error.c_str());
    }
    installed->refresh();
}

int Tasks::worker(unsigned int, void* arg)
{
    Tasks* self = *(Tasks**)arg;
    while (!self->quit) {
        std::shared_ptr<Task> next;
        {
            std::lock_guard<Mutex> lock(self->m);
            for (auto& t : self->tasks) {
                if (t->state == TS_QUEUED) {
                    next = t;
                    break;
                }
            }
        }
        if (!next) {
            sceKernelDelayThread(200 * 1000);
            continue;
        }
        if (next->progress.cancel) {
            next->state = TS_CANCELED;
            continue;
        }
        next->state = TS_RUNNING;
        self->run(*next);
    }
    return sceKernelExitDeleteThread(0);
}

void Tasks::start(Installed* inst)
{
    installed = inst;
    fs::mkdirs(APP_DIR + "/dl");
    fs::mkdirs(TEMP);
    fs::mkdirs(APP_DIR + "/bg");
    thread = sceKernelCreateThread("vpkgj_tasks", worker, 172, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    Tasks* self = this;
    sceKernelStartThread(thread, sizeof(self), &self);
}

void Tasks::stop()
{
    quit = true;
    {
        std::lock_guard<Mutex> lock(m);
        for (auto& t : tasks)
            t->progress.cancel = true;
    }
    if (thread >= 0) {
        SceUInt timeout = 3 * 1000 * 1000;
        sceKernelWaitThreadEnd(thread, nullptr, &timeout);
    }
}

std::shared_ptr<Task> Tasks::add(const Entry& e, const std::vector<Entry>& deps)
{
    std::lock_guard<Mutex> lock(m);
    for (auto& t : tasks)
        if (t->entry.cat == e.cat && t->entry.id == e.id && t->entry.region == e.region && t->entry.content == e.content &&
            (t->state == TS_QUEUED || t->state == TS_RUNNING))
            return t;
    auto t = std::make_shared<Task>();
    t->entry = e;
    t->deps = deps;
    t->stage = "В очереди";
    tasks.push_back(t);
    return t;
}

std::shared_ptr<Task> Tasks::find(const Entry& e)
{
    std::lock_guard<Mutex> lock(m);
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it)
        if ((*it)->entry.cat == e.cat && (*it)->entry.id == e.id && (*it)->entry.region == e.region &&
            (*it)->entry.content == e.content)
            return *it;
    return nullptr;
}

std::vector<std::shared_ptr<Task>> Tasks::list()
{
    std::lock_guard<Mutex> lock(m);
    return tasks;
}

void Tasks::clear_finished()
{
    std::lock_guard<Mutex> lock(m);
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const std::shared_ptr<Task>& t) {
                    return t->state == TS_DONE || t->state == TS_FAILED || t->state == TS_CANCELED;
                }),
                tasks.end());
}

int Tasks::active()
{
    std::lock_guard<Mutex> lock(m);
    int n = 0;
    for (auto& t : tasks)
        if (t->state == TS_QUEUED || t->state == TS_RUNNING)
            n++;
    return n;
}
