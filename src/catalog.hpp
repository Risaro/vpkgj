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

    bool load_cache();
    bool parse(const std::string& json, std::string& err);
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

const char* category_key(Category c);
const char* category_title(Category c);
