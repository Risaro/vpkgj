#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Backup {
    std::string dir;
    std::string label;
    std::string date;
    uint64_t size = 0;
    std::vector<std::pair<std::string, std::string>> items;
};

bool backup_create(const std::string& label, const std::vector<std::string>& paths, std::string& err);
std::vector<Backup> backup_list();
bool backup_restore(const Backup& b, std::string& err);
bool backup_delete(const Backup& b);
uint64_t tree_size(const std::string& path);
std::string savedata_path(const std::string& titleid);
