#include "catalog.hpp"

#include "net.hpp"
#include "promote.hpp"
#include "util.hpp"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

const char* category_key(Category c)
{
    static const char* keys[] = {"games", "ports", "translations", "homebrew", "plugins"};
    return keys[c];
}

const char* category_title(Category c)
{
    static const char* titles[] = {"Игры", "Порты", "Переводы", "Хоумбрю", "Плагины"};
    return titles[c];
}

void Settings::load()
{
    std::string s;
    if (!fs::read(APP_DIR + "/config.txt", s))
        return;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k == "repo" && !v.empty())
            repo = v;
        else if (k == "branch" && !v.empty())
            branch = v;
    }
}

void Settings::save() const
{
    fs::write(APP_DIR + "/config.txt", "repo=" + repo + "\nbranch=" + branch + "\n");
}

std::string Settings::base() const
{
    return "https://raw.githubusercontent.com/" + repo + "/" + branch + "/";
}

static std::string str(const Json::Value& v, const char* key)
{
    const Json::Value& x = v[key];
    if (x.isString())
        return x.asString();
    if (x.isNumeric())
        return x.asString();
    return "";
}

bool Catalog::parse(const std::string& json, std::string& err)
{
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string errs;
    std::istringstream in(json);
    if (!Json::parseFromStream(b, in, &root, &errs) || !root.isObject()) {
        err = "Каталог повреждён";
        return false;
    }
    const Json::Value& cats = root["categories"];
    if (!cats.isObject()) {
        err = "В каталоге нет категорий";
        return false;
    }
    for (int c = 0; c < CAT_COUNT; c++) {
        items[c].clear();
        const Json::Value& arr = cats[category_key((Category)c)];
        if (!arr.isArray())
            continue;
        for (const auto& v : arr) {
            Entry e;
            e.cat = (Category)c;
            e.id = c == CAT_PLUGINS ? str(v, "id") : str(v, "titleid");
            e.name = str(v, "name");
            e.version = str(v, "version");
            e.author = str(v, "author");
            e.lang = str(v, "lang");
            e.description = str(v, "description");
            e.region = str(v, "region");
            e.game_version = str(v, "game_version");
            e.install = str(v, "install");
            e.tai_section = str(v, "tai_section");
            e.url = c == CAT_PORTS || c == CAT_HOMEBREW ? str(v, "vpk") : c == CAT_GAMES ? str(v, "pkg") : str(v, "url");
            e.data = str(v, "data");
            e.data_path = str(v, "data_path");
            e.mirror = str(v, "mirror");
            e.zrif = str(v, "zrif");
            e.sha256 = str(v, "sha256");
            e.content = str(v, "content_id");
            e.alt_url = str(v, "alt_vpk");
            e.alt_version = str(v, "alt_version");
            e.alt_source = str(v, "alt_source");
            e.needs = str(v, "requires");
            e.check = str(v, "check");
            e.size = v["size"].isNumeric() ? v["size"].asUInt64() : 0;
            e.icon = str(v, "icon");
            if (v["shots"].isArray())
                for (const auto& s : v["shots"])
                    if (s.isString())
                        e.shots.push_back(s.asString());
            size_t nl;
            while ((nl = e.description.find("\\n")) != std::string::npos)
                e.description.replace(nl, 2, "\n");
            e.search = lower(e.name + " " + e.id + " " + e.author);
            e.sort_name = lower(e.name);
            e.date = str(v, "date");
            if (e.date.empty())
                e.date = str(v, "updated");
            if (!e.id.empty() && !e.name.empty())
                items[c].push_back(std::move(e));
        }
    }
    version = str(root, "version");
    media = str(root, "media");
    const Json::Value& app = root["app"];
    if (app.isObject()) {
        app_version = str(app, "version");
        app_url = str(app, "vpk");
        if (app_url.empty())
            app_url = str(app, "url");
        app_notes = str(app, "notes");
    }
    return true;
}

bool Catalog::load_cache()
{
    std::string s, err;
    if (!fs::read(APP_DIR + "/index.json", s))
        return false;
    return parse(s, err);
}

std::string Catalog::media_url(const std::string& path) const
{
    if (path.empty() || path.find(':') != std::string::npos)
        return path;
    return media + path;
}

static std::string key(const Entry& e)
{
    std::string k = std::string(category_key(e.cat)) + "/" + e.id;
    if (e.dlc)
        k += "/" + e.content;
    return k;
}

std::string pkgj_config_path()
{
    for (const char* p : {"ux0:pkgj/config.txt", "ur0:pkgj/config.txt"})
        if (fs::exists(p))
            return p;
    return "";
}

static std::string pkgj_updates_url;

bool pkgj_urls(std::string& games, std::string& dlcs)
{
    std::string path = pkgj_config_path();
    std::string text;
    if (path.empty() || !fs::read(path, text))
        return false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k, v;
        ls >> k >> v;
        k = lower(k);
        if ((k == "url" || k == "url_games") && !v.empty())
            games = v;
        else if (k == "url_dlcs" && !v.empty())
            dlcs = v;
        else if (k == "url_updates" && !v.empty())
            pkgj_updates_url = v;
    }
    return !games.empty() || !dlcs.empty();
}

#define PKGJ_GAMES (APP_DIR + "/pkgj_games.tsv")
#define PKGJ_DLCS (APP_DIR + "/pkgj_dlcs.tsv")
#define PKGJ_UPDATES (APP_DIR + "/pkgj_updates.tsv")

void pkgj_fetch(bool force)
{
    std::string games, dlcs;
    if (!pkgj_urls(games, dlcs))
        return;
    std::string err;
    if (!games.empty() && (force || !fs::exists(PKGJ_GAMES)))
        if (!net::fetch(games, PKGJ_GAMES, nullptr, err))
            log_line("pkgj games list: %s", err.c_str());
    if (!dlcs.empty() && (force || !fs::exists(PKGJ_DLCS)))
        if (!net::fetch(dlcs, PKGJ_DLCS, nullptr, err))
            log_line("pkgj dlcs list: %s", err.c_str());
    std::string stamp;
    fs::read(APP_DIR + "/pkgj_updates.time", stamp);
    uint64_t at = strtoull(stamp.c_str(), nullptr, 10), now = (uint64_t)time(nullptr);
    bool fresh = fs::exists(PKGJ_UPDATES) && at && now >= at && now - at < 12 * 3600;
    if (!pkgj_updates_url.empty() && (force || !fresh)) {
        if (net::fetch(pkgj_updates_url, PKGJ_UPDATES, nullptr, err))
            fs::write(APP_DIR + "/pkgj_updates.time", std::to_string(now));
        else
            log_line("pkgj updates list: %s", err.c_str());
    }
}

static std::vector<std::string> split_tab(const std::string& line)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = line.find('\t', a);
        out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos)
            break;
        a = b + 1;
    }
    return out;
}

static int load_nps(std::vector<Entry>& out, const std::string& path, bool dlc)
{
    std::string text;
    if (!fs::read(path, text))
        return 0;
    int n = 0;
    size_t pos = text.find('\n');
    while (pos != std::string::npos && pos + 1 < text.size()) {
        size_t end = text.find('\n', pos + 1);
        std::string line = text.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        pos = end;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto f = split_tab(line);
        if (f.size() < 9)
            continue;
        const std::string& url = f[3];
        const std::string& zrif = f[4];
        if (url.empty() || url == "MISSING" || url == "CART ONLY" || zrif == "MISSING" || f[0].size() != 9)
            continue;
        Entry e;
        e.cat = CAT_GAMES;
        e.id = f[0];
        e.region = f[1];
        e.name = f[2];
        e.url = url;
        e.zrif = zrif == "NOT REQUIRED" ? "" : zrif;
        e.content = f[5];
        e.dlc = dlc;
        std::string size = dlc ? f[7] : f[8];
        std::string sha = dlc ? f[8] : (f.size() > 9 ? f[9] : "");
        e.size = strtoull(size.c_str(), nullptr, 10);
        e.sha256 = sha.size() == 64 ? sha : "";
        if (dlc)
            e.author = "DLC";
        else if (f.size() > 10 && !f[10].empty())
            e.version = "FW " + f[10];
        e.search = lower(e.name + " " + e.id + (dlc ? " dlc" : ""));
        e.sort_name = lower(e.name);
        e.date = f[6];
        out.push_back(std::move(e));
        n++;
    }
    return n;
}

void Catalog::apply_covers()
{
    std::string text;
    bool bundled = fs::exists("app0:covers.pak");
    if (!fs::read(APP_DIR + "/covers.json", text) && !fs::read("app0:covers.json", text))
        return;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string errs;
    std::istringstream in(text);
    if (!Json::parseFromStream(b, in, &root, &errs) || !root.isObject())
        return;
    for (auto& e : items[CAT_GAMES]) {
        const Json::Value& v = root[e.id];
        if (!v.isObject())
            continue;
        if (e.icon.empty()) {
            std::string c = str(v, "c");
            e.icon = bundled && starts_with(c, "covers/") ? "pak:" + c : c;
        }
        if (e.shots.empty() && v["s"].isArray())
            for (const auto& s : v["s"])
                if (s.isString())
                    e.shots.push_back(s.asString());
        if (e.description.empty())
            e.description = str(v, "d");
        if (e.lang.empty())
            e.lang = str(v, "l");
    }
}

int version_cmp(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && !isdigit((unsigned char)a[i]))
            i++;
        while (i < a.size() && isdigit((unsigned char)a[i]))
            x = x * 10 + (a[i++] - '0');
        while (j < b.size() && !isdigit((unsigned char)b[j]))
            j++;
        while (j < b.size() && isdigit((unsigned char)b[j]))
            y = y * 10 + (b[j++] - '0');
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

void Catalog::load_updates()
{
    patches.clear();
    game_versions.clear();
    std::string text;
    if (fs::read(PKGJ_UPDATES, text)) {
        size_t pos = text.find('\n');
        while (pos != std::string::npos && pos + 1 < text.size()) {
            size_t end = text.find('\n', pos + 1);
            std::string line = text.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
            pos = end;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto f = split_tab(line);
            if (f.size() < 9 || f[0].size() != 9 || f[5].empty() || f[5] == "MISSING")
                continue;
            auto it = patches.find(f[0]);
            if (it != patches.end() && version_cmp(it->second.version, f[3]) >= 0)
                continue;
            Entry e;
            e.cat = CAT_GAMES;
            e.id = f[0];
            e.region = f[1];
            e.name = "Обновление " + f[2];
            e.version = f[3];
            e.url = f[5];
            e.size = strtoull(f[7].c_str(), nullptr, 10);
            e.sha256 = f[8].size() == 64 ? f[8] : "";
            e.install = "patch";
            patches[f[0]] = e;
        }
    }
    std::string out;
    for (auto& id : fs::list("ux0:app")) {
        if (id.size() != 9 || !patches.count(id))
            continue;
        std::string v;
        if (sfo_get("ux0:patch/" + id + "/sce_sys/param.sfo", "APP_VER", v) || sfo_get("ux0:app/" + id + "/sce_sys/param.sfo", "APP_VER", v)) {
            game_versions[id] = v;
            out += id + "\t" + v + "\n";
        }
    }
    std::string old;
    fs::read(APP_DIR + "/game_versions.txt", old);
    if (old != out)
        fs::write(APP_DIR + "/game_versions.txt", out);
}

void Catalog::prepare()
{
    std::set<std::string> tr;
    for (auto& t : items[CAT_TRANSLATIONS])
        tr.insert(t.id);
    for (int c = 0; c < CAT_COUNT; c++)
        for (auto& e : items[c]) {
            if (e.sort_name.empty())
                e.sort_name = lower(e.name);
            e.ru = lower(e.lang).find("ru") != std::string::npos || (c != CAT_TRANSLATIONS && tr.count(e.id));
        }
}

const Entry* Catalog::patch_for(const Entry& e) const
{
    if (e.cat != CAT_GAMES || e.dlc || e.install == "patch")
        return nullptr;
    auto v = game_versions.find(e.id);
    auto p = patches.find(e.id);
    if (v == game_versions.end() || p == patches.end())
        return nullptr;
    return version_cmp(v->second, p->second.version) < 0 ? &p->second : nullptr;
}

void Catalog::load_pkgj()
{
    auto& g = items[CAT_GAMES];
    std::string games = fs::exists(PKGJ_GAMES) ? PKGJ_GAMES : "ux0:pkgj/titles_psvgames.tsv";
    std::string dlcs = fs::exists(PKGJ_DLCS) ? PKGJ_DLCS : "ux0:pkgj/titles_psvdlcs.tsv";
    pkgj_games = load_nps(g, games, false);
    pkgj_dlcs = load_nps(g, dlcs, true);
}

void Installed::load()
{
    std::lock_guard<Mutex> lock(m);
    std::string s;
    if (!fs::read(APP_DIR + "/installed.txt", s))
        return;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        size_t t = line.find('\t');
        if (t != std::string::npos)
            versions[line.substr(0, t)] = trim(line.substr(t + 1));
    }
}

void Installed::save()
{
    std::string s;
    for (auto& kv : versions)
        s += kv.first + "\t" + kv.second + "\n";
    fs::write(APP_DIR + "/installed.txt", s);
}

void Installed::set(const Entry& e)
{
    std::lock_guard<Mutex> lock(m);
    versions[key(e)] = e.version.empty() ? "-" : e.version;
    scanned = false;
    save();
}

void Installed::remove(const Entry& e)
{
    std::lock_guard<Mutex> lock(m);
    versions.erase(key(e));
    scanned = false;
    save();
}

void Installed::refresh()
{
    std::lock_guard<Mutex> lock(m);
    scanned = false;
}

void Installed::scan()
{
    present.clear();
    for (auto& id : fs::list("ux0:app"))
        present.insert(id);
    for (auto& id : fs::list("ux0:addcont"))
        for (auto& sub : fs::list("ux0:addcont/" + id))
            present.insert("dlc:ux0:addcont/" + id + "/" + sub);
    for (auto& id : fs::list("ux0:rePatch"))
        present.insert("rp:" + id);
    found.clear();
    scanned = true;
}

bool Installed::detect(const Entry& e)
{
    if (!scanned)
        scan();
    std::string k = key(e);
    auto it = found.find(k);
    if (it != found.end())
        return it->second;
    bool r = false;
    if (e.cat == CAT_PLUGINS) {
        r = plugin_present(e);
    } else if (e.cat == CAT_TRANSLATIONS) {
        std::string type = lower(e.install);
        if (type == "data")
            r = !e.data_path.empty() && fs::exists(e.data_path);
        else if (type == "plugin")
            r = plugin_present(e);
        else if (type != "vpk")
            r = present.count("rp:" + e.id) > 0;
    }
    found[k] = r;
    return r;
}

bool Installed::app_exists(const std::string& titleid)
{
    if (!scanned)
        scan();
    return present.count(titleid) > 0;
}

Status Installed::status(const Entry& e)
{
    std::lock_guard<Mutex> lock(m);
    auto it = versions.find(key(e));
    bool recorded = it != versions.end();
    if (e.cat == CAT_GAMES && e.dlc) {
        std::string p = e.content.size() >= 36 ? "ux0:addcont/" + e.id + "/" + e.content.substr(20, 16) : "";
        return !p.empty() && app_exists("dlc:" + p) ? ST_INSTALLED : ST_NONE;
    }
    if (e.cat == CAT_PORTS || e.cat == CAT_HOMEBREW || e.cat == CAT_GAMES) {
        if (!app_exists(e.id))
            return ST_NONE;
        if (e.cat == CAT_GAMES)
            return ST_INSTALLED;
        if (recorded && it->second != (e.version.empty() ? "-" : e.version) && it->second != e.alt_version)
            return ST_UPDATE;
        return ST_INSTALLED;
    }
    if (!recorded)
        return detect(e) ? ST_INSTALLED : ST_NONE;
    if (e.cat == CAT_PLUGINS && e.id == "vpkgj_daemon")
        return version_cmp(it->second, e.version) < 0 ? ST_UPDATE : ST_INSTALLED;
    if (it->second != (e.version.empty() ? "-" : e.version) && it->second != e.alt_version)
        return ST_UPDATE;
    return ST_INSTALLED;
}

std::string Installed::version_of(const Entry& e)
{
    std::lock_guard<Mutex> lock(m);
    auto it = versions.find(key(e));
    return it == versions.end() ? "" : it->second;
}
