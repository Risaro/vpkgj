#pragma once

#include "net.hpp"

#include <cstdint>
#include <string>
#include <vector>

bool zrif_decode(const std::string& zrif, std::vector<uint8_t>& rif, std::string& err);

bool pkg_install(const std::string& url, const std::string& zrif, const std::string& sha256,
                 const std::string& workdir, net::Progress* progress, std::string& stage, std::string& err);
