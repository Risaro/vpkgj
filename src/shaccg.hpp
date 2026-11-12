#pragma once

#include <string>

namespace shaccg {
enum State { SH_OK, SH_DIR, SH_MISSING };
extern const char* SHARK_ID;
extern const char* PSM_ID;
State state();
bool psm_installed();
bool shark_installed();
bool armed();
bool arm(std::string& err);
void disarm();
bool load_kernel();
int tries();
void add_try();
void reset_tries();
}
