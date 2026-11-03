#pragma once

#include "net.hpp"

#include <functional>
#include <string>
#include <vector>

using ZipFilter = std::function<std::string(const std::string& name)>;

bool zip_list(const std::string& path, std::vector<std::string>& names, std::string& err);
bool zip_extract(const std::string& path, const std::string& dest, net::Progress* p, std::string& err,
                 const ZipFilter& map = nullptr);
