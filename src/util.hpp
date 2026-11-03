#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fs {
bool exists(const std::string& path);
bool is_dir(const std::string& path);
bool mkdirs(const std::string& path);
bool rmtree(const std::string& path);
bool read(const std::string& path, std::string& out);
bool write(const std::string& path, const void* data, size_t size);
bool write(const std::string& path, const std::string& data);
int64_t size(const std::string& path);
uint64_t free_space(const std::string& device);
std::vector<std::string> list(const std::string& path);
std::string parent(const std::string& path);
std::string device(const std::string& path);
}

std::string fmt_size(uint64_t bytes);
std::string trim(const std::string& s);
std::string lower(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
std::string hash_name(const std::string& s);
uint64_t now_ms();
void log_line(const char* fmt, ...);

extern const std::string APP_DIR;
