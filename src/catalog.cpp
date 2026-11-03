#include "catalog.hpp"

#include "util.hpp"

#include <json/json.h>

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
            if (!e.id.empty() && !e.name.empty())
                items[c].push_back(std::move(e));
        }
    }
    version = str(root, "version");
    media = str(root, "media");
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
    if (path.empty() || starts_with(path, "http://") || starts_with(path, "https://"))
        return path;
    return media + path;
}

static std::string key(const Entry& e)
{
    return std::string(category_key(e.cat)) + "/" + e.id;
}

void Installed::load()
{
    std::lock_guard<std::mutex> lock(m);
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
    std::lock_guard<std::mutex> lock(m);
    versions[key(e)] = e.version.empty() ? "-" : e.version;
    app_cache.clear();
    save();
}

void Installed::remove(const Entry& e)
{
    std::lock_guard<std::mutex> lock(m);
    versions.erase(key(e));
    app_cache.clear();
    save();
}

void Installed::refresh()
{
    std::lock_guard<std::mutex> lock(m);
    app_cache.clear();
}

bool Installed::app_exists(const std::string& titleid)
{
    auto it = app_cache.find(titleid);
    if (it != app_cache.end())
        return it->second;
    bool ok = fs::exists("ux0:app/" + titleid + "/eboot.bin");
    app_cache[titleid] = ok;
    return ok;
}

Status Installed::status(const Entry& e)
{
    std::lock_guard<std::mutex> lock(m);
    auto it = versions.find(key(e));
    bool recorded = it != versions.end();
    if (e.cat == CAT_PORTS || e.cat == CAT_HOMEBREW || e.cat == CAT_GAMES) {
        if (!app_exists(e.id))
            return ST_NONE;
        if (e.cat == CAT_GAMES)
            return ST_INSTALLED;
        if (recorded && it->second != (e.version.empty() ? "-" : e.version))
            return ST_UPDATE;
        return ST_INSTALLED;
    }
    if (!recorded)
        return ST_NONE;
    if (it->second != (e.version.empty() ? "-" : e.version))
        return ST_UPDATE;
    return ST_INSTALLED;
}
