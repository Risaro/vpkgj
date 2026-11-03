#pragma once

#include <string>

namespace ime {
bool open(const std::string& title, const std::string& initial, int max_len);
bool active();
int poll(std::string& out);
}
