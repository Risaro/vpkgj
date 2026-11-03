#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum BgdlType { BGDL_GAME = 0x16, BGDL_DLC = 0x17 };

bool bgdl_start(int type, const std::string& title, const std::string& url, const std::vector<uint8_t>& rif, std::string& err);
