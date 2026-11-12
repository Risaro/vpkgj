#include "shaccg.hpp"

#include "tasks.hpp"
#include "util.hpp"

#include <psp2/io/fcntl.h>
#include <taihen.h>

#include <cstdlib>

namespace shaccg {

const char* SHARK_ID = "SHARKF00D";
const char* PSM_ID = "PCSI00011";

static const char* LIB = "ur0:data/libshacccg.suprx";
static const char* LIB_EXT = "ur0:data/external/libshacccg.suprx";
static const std::string USER_PRX = "ux0:data/vpkgj_psm.suprx";
static const std::string KERNEL_PRX = "ux0:data/vpkgj_psm.skprx";
static const std::string HEADER = "*SHARKF00D\n" + USER_PRX + "\n*NPXS10031\n" + USER_PRX + "\n";

static bool lib_file(const char* p)
{
    return fs::exists(p) && !fs::is_dir(p) && fs::size(p) > 0;
}

State state()
{
    if (lib_file(LIB) || lib_file(LIB_EXT))
        return SH_OK;
    if (fs::is_dir(LIB))
        return SH_DIR;
    return SH_MISSING;
}

bool psm_installed()
{
    return fs::exists(std::string("ux0:app/") + PSM_ID + "/runtime_version.txt");
}

bool shark_installed()
{
    return fs::exists(std::string("ux0:app/") + SHARK_ID + "/eboot.bin");
}

static std::string cfg_path()
{
    return tai_dir() + "/config.txt";
}

bool armed()
{
    std::string s;
    return fs::read(cfg_path(), s) && starts_with(s, HEADER);
}

static bool copy(const std::string& from, const std::string& to)
{
    std::string d;
    return fs::read(from, d) && !d.empty() && fs::write(to, d);
}

bool arm(std::string& err)
{
    if (!copy("app0:vpkgj_psm.suprx", USER_PRX) || !copy("app0:vpkgj_psm.skprx", KERNEL_PRX)) {
        err = "Не удалось скопировать помощник установки в ux0:data";
        return false;
    }
    if (armed())
        return true;
    std::string s;
    if (!fs::read(cfg_path(), s)) {
        err = "Не читается " + cfg_path();
        return false;
    }
    if (!fs::write(cfg_path(), HEADER + s)) {
        err = "Не удалось записать " + cfg_path();
        return false;
    }
    return true;
}

void disarm()
{
    std::string s;
    if (fs::read(cfg_path(), s) && starts_with(s, HEADER))
        fs::write(cfg_path(), s.substr(HEADER.size()));
    sceIoRemove(USER_PRX.c_str());
    sceIoRemove(KERNEL_PRX.c_str());
    sceIoRemove("ux0:data/Runtime1.00.pkg");
    sceIoRemove("ux0:data/Runtime2.01.pkg");
    reset_tries();
}

bool load_kernel()
{
    SceUID r = taiLoadStartKernelModule(KERNEL_PRX.c_str(), 0, nullptr, 0);
    log_line("psm kernel helper: %08x", (unsigned)r);
    return r >= 0;
}

int tries()
{
    std::string s;
    fs::read(APP_DIR + "/shaccg_try.txt", s);
    return atoi(trim(s).c_str());
}

void add_try()
{
    fs::write(APP_DIR + "/shaccg_try.txt", std::to_string(tries() + 1));
}

void reset_tries()
{
    sceIoRemove((APP_DIR + "/shaccg_try.txt").c_str());
}

}
