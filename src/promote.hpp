#pragma once

#include <string>

void promoter_init();
void promoter_fini();
bool sfo_get(const std::string& path, const std::string& key, std::string& out);
bool make_head_bin(const std::string& dir, std::string& err);
bool promote_app(const std::string& dir, std::string& err);
bool promote_pkg(const std::string& dir, std::string& err);
