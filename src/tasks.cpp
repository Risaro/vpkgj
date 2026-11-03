#include "tasks.hpp"

#include "backup.hpp"
#include "pkg.hpp"
#include "promote.hpp"
#include "util.hpp"
#include "zip.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <sstream>

static const std::string TEMP = "ux0:temp/vpkgj";

std::string Task::get_stage()
{
    std::lock_guard<std::mutex> lock(m);
    return stage;
}

std::string Task::get_error()
{
    std::lock_guard<std::mutex> lock(m);
    return error;
}

void Task::set_stage(const std::string& s)
{
    std::lock_guard<std::mutex> lock(m);
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

static bool download(Task& t, const std::vector<std::string>& urls, const std::string& path, std::string& err)
{
    if (urls.empty()) {
        err = "Нет ссылки на файл";
        return false;
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

static bool is_zip(const std::string& path)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0)
        return false;
    char b[2] = {0, 0};
    sceIoRead(f, b, 2);
    sceIoClose(f);
    return b[0] == 'P' && b[1] == 'K';
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
    std::lock_guard<std::mutex> lock(t.m);
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
            if (fs::exists(k.substr(5)))
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
            std::lock_guard<std::mutex> lock(t.m);
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
        std::lock_guard<std::mutex> lock(t.m);
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
        std::lock_guard<std::mutex> lock(t.m);
        t.note = "Перезагрузи консоль, чтобы плагин включился";
    t.reboot = true;
    }
    return true;
}

void Tasks::run(Task& t)
{
    const Entry& e = t.entry;
    std::string err;
    std::string dl = APP_DIR + "/dl/" + std::string(category_key(e.cat)) + "_" + hash_name(e.id + e.url);
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
        if (enough_space(e, 2.1, err) && download(t, links(e.url, e.data.empty() ? e.mirror : ""), dl, err)) {
            ok = install_vpk(t, dl, err);
            sceIoRemove(dl.c_str());
            std::vector<std::string> parts = split(e.data, ';');
            for (size_t i = 0; ok && i < parts.size(); i++) {
                std::string src = parts[i], dest = e.data_path;
                size_t gt = src.find('>');
                if (gt != std::string::npos) {
                    dest = trim(src.substr(gt + 1));
                    src = trim(src.substr(0, gt));
                }
                std::string dd = dl + "_data" + std::to_string(i);
                ok = download(t, links(src, i == 0 ? e.mirror : ""), dd, err) && install_data(t, dd, dest, err);
                sceIoRemove(dd.c_str());
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
                ok = download(t, links(parts[i], i == 0 ? e.mirror : ""), f, err) && install_translation(t, f, err);
                sceIoRemove(f.c_str());
            }
        }
    } else if (e.cat == CAT_PLUGINS) {
        if (download(t, links(e.url, e.mirror), dl, err)) {
            ok = install_plugin_files(t, e, dl, e.tai_section, err);
            sceIoRemove(dl.c_str());
        }
    }
    if (ok) {
        installed->set(e);
        t.set_stage("Готово");
        t.state = TS_DONE;
    } else {
        std::lock_guard<std::mutex> lock(t.m);
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
            std::lock_guard<std::mutex> lock(self->m);
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
    thread = sceKernelCreateThread("vpkgj_tasks", worker, 0x10000100, 256 * 1024, 0, 0, nullptr);
    Tasks* self = this;
    sceKernelStartThread(thread, sizeof(self), &self);
}

void Tasks::stop()
{
    quit = true;
    {
        std::lock_guard<std::mutex> lock(m);
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
    std::lock_guard<std::mutex> lock(m);
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
    std::lock_guard<std::mutex> lock(m);
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it)
        if ((*it)->entry.cat == e.cat && (*it)->entry.id == e.id && (*it)->entry.region == e.region &&
            (*it)->entry.content == e.content)
            return *it;
    return nullptr;
}

std::vector<std::shared_ptr<Task>> Tasks::list()
{
    std::lock_guard<std::mutex> lock(m);
    return tasks;
}

void Tasks::clear_finished()
{
    std::lock_guard<std::mutex> lock(m);
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const std::shared_ptr<Task>& t) {
                    return t->state == TS_DONE || t->state == TS_FAILED || t->state == TS_CANCELED;
                }),
                tasks.end());
}

int Tasks::active()
{
    std::lock_guard<std::mutex> lock(m);
    int n = 0;
    for (auto& t : tasks)
        if (t->state == TS_QUEUED || t->state == TS_RUNNING)
            n++;
    return n;
}
