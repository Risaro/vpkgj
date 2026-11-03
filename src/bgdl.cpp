#include "bgdl.hpp"

#include "util.hpp"

#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

struct DownloadParam {
    int type[2];
    char unk_0x08[0x68];
    char url[0x800];
    char icon_path[0x100];
    char title[0x33A];
    char license_path[0x100];
    char unk_0xDAA[0x16];
};

struct IpmiParam {
    union {
        struct {
            uint32_t* ptr_to_dc0_ptr;
            uint32_t* ptr_to_2e0_ptr;
            uint32_t unk_1;
            uint32_t unk_2;
            uint32_t unk_3;
            DownloadParam* addr_dc0;
            uint32_t size_dc0;
        } init;
        struct {
            int32_t* result;
            uint32_t unk_2;
            uint32_t unk_3;
            uint32_t unk_4;
            uint32_t unk_5;
            uint32_t unk_6;
            uint32_t unk_7;
        } state;
    };
    void* addr_2e0;
    uint32_t size_2e0;
    uint32_t unk_4;
    uint32_t* bgdl_id;
    uint32_t unk_5;
    int32_t* result;
    uint32_t unk_4_2;
    uint32_t shell_func_8;
};

struct ShellInit {
    uint32_t unk_0;
    char name[0x10];
    void* unk_ptr;
    uint32_t unk_1;
    uint32_t size1;
    uint32_t size2;
    uint32_t unk_2;
    uint32_t unk_3;
    uint32_t unk_4;
    uint32_t unk_5;
    char padding[0x84];
    uint32_t unk_7;
    uint32_t unk_8;
    void* unk_ptr_2;
    char padding2[0x88];
};

struct ClassHeader {
    uint32_t unk0;
    uint32_t unk1;
    uint32_t unk2;
    uint32_t** func_table;
    uint32_t unk3;
    uint32_t* buf_c4;
    uint32_t* buf_10000;
};

typedef int (*InitFn)(uint32_t**, void*, int, ShellInit*, int);
typedef int (*ChangeStateFn)(uint32_t**, int, void*, int, IpmiParam);

struct Downloader {
    ShellInit header;
    ClassHeader* cls;
    InitFn init;
    ChangeStateFn change_state;
};

int (*ipmi_create)(const char* name, int unk) = nullptr;
int (*ipmi_init)(uint32_t*** func_table, const char* name, ClassHeader* cls, uint32_t* buf) = nullptr;

std::string hexerr(const char* what, int v)
{
    char b[96];
    snprintf(b, sizeof(b), "%s: 0x%08X", what, (unsigned)v);
    return b;
}

Downloader* open(std::string& err)
{
    static Downloader* d = nullptr;
    if (d)
        return d;
    sceKernelLoadStartModule("vs0:sys/external/libshellsvc.suprx", 0, nullptr, 0, nullptr, nullptr);
    taiGetModuleExportFunc("SceShellSvc", 0xF4E34EDB, 0x4E255C31, (uintptr_t*)&ipmi_create);
    taiGetModuleExportFunc("SceShellSvc", 0xF4E34EDB, 0xB282B430, (uintptr_t*)&ipmi_init);
    if (!ipmi_create || !ipmi_init) {
        err = "Системная загрузка недоступна";
        return nullptr;
    }
    std::unique_ptr<Downloader> o(new Downloader());
    memset(o.get(), 0, sizeof(Downloader));
    strncpy(o->header.name, "SceDownload", sizeof(o->header.name));
    o->header.unk_1 = 1;
    o->header.size1 = 0x1E00;
    o->header.size2 = 0x1E00;
    o->header.unk_2 = 1;
    o->header.unk_3 = 0x0F00;
    o->header.unk_4 = 0x0F00;
    o->header.unk_5 = 1;
    o->header.unk_7 = 2;
    o->header.unk_8 = (uint32_t)-1;
    int res = ipmi_create(o->header.name, 0x1E00);
    if (res != 0xc4) {
        err = hexerr("SceIpmi create", res);
        return nullptr;
    }
    o->cls = (ClassHeader*)calloc(1, sizeof(ClassHeader));
    o->cls->buf_c4 = (uint32_t*)calloc(1, res);
    o->cls->buf_10000 = (uint32_t*)calloc(1, 0x1000);
    res = ipmi_init(&o->cls->func_table, o->header.name, o->cls, o->cls->buf_10000);
    if (res != 0) {
        err = hexerr("SceIpmi init", res);
        return nullptr;
    }
    o->init = (InitFn)(*o->cls->func_table)[1];
    o->change_state = (ChangeStateFn)(*o->cls->func_table)[5];
    res = o->init(o->cls->func_table, *o->cls->func_table, 0x14, &o->header, 2);
    if (res != 0) {
        err = hexerr("SceDownload init", res);
        return nullptr;
    }
    d = o.release();
    return d;
}

}

bool bgdl_start(int type, const std::string& title, const std::string& url, const std::vector<uint8_t>& rif, std::string& err)
{
    if (fs::list("ux0:bgdl/t").size() >= 32) {
        err = "Слишком много ожидающих установок, поставь их из уведомлений";
        return false;
    }
    Downloader* d = open(err);
    if (!d)
        return false;
    std::string license;
    if (!rif.empty()) {
        license = "ux0:bgdl/temp.dat";
        if (!fs::write(license, rif.data(), std::min<size_t>(rif.size(), 512))) {
            err = "Не удалось записать лицензию";
            return false;
        }
    }
    int32_t result = 0;
    uint32_t bgdl_id = 1;
    std::vector<uint8_t> dc0(0xDC0), e2(0x2E0);
    IpmiParam p;
    memset(&p, 0, sizeof(p));
    p.init.ptr_to_dc0_ptr = (uint32_t*)&p.init.addr_dc0;
    p.init.ptr_to_2e0_ptr = (uint32_t*)&p.addr_2e0;
    p.init.unk_1 = 2;
    p.init.unk_2 = (uint32_t)-1;
    p.init.size_dc0 = dc0.size();
    p.init.addr_dc0 = (DownloadParam*)dc0.data();
    p.size_2e0 = e2.size();
    p.addr_2e0 = e2.data();
    p.bgdl_id = &bgdl_id;
    p.unk_5 = 4;
    p.result = &result;
    p.shell_func_8 = (*d->cls->func_table)[8];
    snprintf(p.init.addr_dc0->url, sizeof(p.init.addr_dc0->url), "%s", url.c_str());
    snprintf(p.init.addr_dc0->license_path, sizeof(p.init.addr_dc0->license_path), "%s", license.c_str());
    snprintf(p.init.addr_dc0->title, sizeof(p.init.addr_dc0->title), "%s", title.c_str());
    snprintf(p.init.addr_dc0->icon_path, sizeof(p.init.addr_dc0->icon_path), "%s", "ux0:bgdl/icon0.png");
    p.init.addr_dc0->type[0] = p.init.addr_dc0->type[1] = type;
    int res = d->change_state(d->cls->func_table, 0x12340012, p.init.ptr_to_dc0_ptr, 1, p);
    if (res < 0 || result < 0) {
        err = hexerr("SceDownload", res < 0 ? res : result);
        return false;
    }
    result = 0;
    memset(&p, 0, sizeof(p));
    p.state.result = &result;
    p.state.unk_4 = 1;
    p.state.unk_7 = 0x00000A0A;
    res = d->change_state(d->cls->func_table, 0x12340007, nullptr, 0, p);
    if (res < 0 || result < 0) {
        err = hexerr("SceDownload start", res < 0 ? res : result);
        return false;
    }
    return true;
}
