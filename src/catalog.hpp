#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

enum Category { CAT_GAMES, CAT_PORTS, CAT_TRANSLATIONS, CAT_HOMEBREW, CAT_PLUGINS, CAT_COUNT };

struct Entry {
    Category cat;
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string lang;
    std::string description;
    std::string region;
    std::string game_version;
    std::string install;
    std::string tai_section;
    std::string url;
    std::string data;
    std::string data_path;
    std::string mirror;
    std::string zrif;
    std::string sha256;
    std::string content;
    std::string needs;
    std::string check;
    bool dlc = false;
    uint64_t size = 0;
    std::string icon;
    std::vector<std::string> shots;
    std::string search;
};

enum Status { ST_NONE, ST_INSTALLED, ST_UPDATE };

struct Settings {
    std::string repo = "Risaro/vpkgj-db";
    std::string branch = "main";
    void load();
    void save() const;
    std::string base() const;
};

class Catalog {
public:
    std::vector<Entry> items[CAT_COUNT];
    std::string version;
    std::string media;

    int pkgj_games = 0;
    int pkgj_dlcs = 0;

    bool load_cache();
    bool parse(const std::string& json, std::string& err);
    void load_pkgj();
    void apply_covers();
    void load_updates();
    const Entry* patch_for(const Entry& e) const;
    std::map<std::string, Entry> patches;
    std::map<std::string, std::string> game_versions;
    std::string media_url(const std::string& path) const;
};

class Installed {
public:
    void load();
    void set(const Entry& e);
    void remove(const Entry& e);
    Status status(const Entry& e);
    void refresh();

private:
    std::mutex m;
    std::map<std::string, std::string> versions;
    std::map<std::string, bool> app_cache;
    void save();
    bool app_exists(const std::string& titleid);
};

std::string pkgj_config_path();
bool pkgj_urls(std::string& games, std::string& dlcs);
void pkgj_fetch(bool force);
int version_cmp(const std::string& a, const std::string& b);

const char* category_key(Category c);
const char* category_title(Category c);
