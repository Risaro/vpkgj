#include "mutex.hpp"
#include "promote.hpp"

#include "headbin.hpp"
#include "util.hpp"

#include <mbedtls/sha1.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

static Mutex promoter_lock;

void promoter_init()
{
    uint32_t args[] = {0x400000, 0xEA60, 0x40000, 0, 1, 0};
    int result = 0;
    SceSysmoduleOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.result = &result;
    sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(args), args, &opt);
    sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    scePromoterUtilityInit();
}

void promoter_fini()
{
    scePromoterUtilityExit();
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
}

static uint32_t le32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t* p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

bool sfo_get(const std::string& path, const std::string& key, std::string& out)
{
    std::string data;
    if (!fs::read(path, data) || data.size() < 20)
        return false;
    const uint8_t* d = (const uint8_t*)data.data();
    if (le32(d) != 0x46535000)
        return false;
    uint32_t keys = le32(d + 8);
    uint32_t vals = le32(d + 12);
    uint32_t count = le32(d + 16);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = d + 20 + i * 16;
        if ((size_t)(e + 16 - d) > data.size())
            return false;
        uint32_t ko = keys + le16(e);
        uint32_t len = le32(e + 4);
        uint32_t vo = vals + le32(e + 12);
        if (ko >= data.size() || vo + len > data.size())
            return false;
        if (key == (const char*)d + ko) {
            out.assign((const char*)d + vo, strnlen((const char*)d + vo, len));
            return true;
        }
    }
    return false;
}

bool sfo_set(const std::string& path, const std::string& key, const std::string& value)
{
    std::string data;
    if (!fs::read(path, data) || data.size() < 20)
        return false;
    uint8_t* d = (uint8_t*)&data[0];
    if (le32(d) != 0x46535000)
        return false;
    uint32_t keys = le32(d + 8);
    uint32_t vals = le32(d + 12);
    uint32_t count = le32(d + 16);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* e = d + 20 + i * 16;
        if ((size_t)(e + 16 - d) > data.size())
            return false;
        uint32_t ko = keys + le16(e);
        uint32_t max = le32(e + 8);
        uint32_t vo = vals + le32(e + 12);
        if (ko >= data.size() || vo + max > data.size())
            return false;
        if (key != (const char*)d + ko)
            continue;
        if (max == 0)
            return false;
        size_t n = value.size() < max - 1 ? value.size() : max - 1;
        while (n > 0 && n < value.size() && ((unsigned char)value[n] & 0xC0) == 0x80)
            n--;
        memset(d + vo, 0, max);
        memcpy(d + vo, value.data(), n);
        uint32_t len = (uint32_t)n + 1;
        e[4] = len & 0xFF;
        e[5] = (len >> 8) & 0xFF;
        e[6] = (len >> 16) & 0xFF;
        e[7] = (len >> 24) & 0xFF;
        return fs::write(path, data);
    }
    return false;
}

static void fpkg_hmac(const uint8_t* data, size_t len, uint8_t out[16])
{
    uint8_t sha[20];
    uint8_t buf[64];
    mbedtls_sha1(data, len, sha);
    memset(buf, 0, sizeof(buf));
    memcpy(&buf[0], &sha[4], 8);
    memcpy(&buf[8], &sha[4], 8);
    memcpy(&buf[16], &sha[12], 4);
    buf[20] = sha[16];
    buf[21] = sha[1];
    buf[22] = sha[2];
    buf[23] = sha[3];
    memcpy(&buf[24], &buf[16], 8);
    mbedtls_sha1(buf, sizeof(buf), sha);
    memcpy(out, sha, 16);
}

bool make_head_bin(const std::string& dir, std::string& err)
{
    std::string head = dir + "/sce_sys/package/head.bin";
    if (fs::exists(head))
        return true;
    std::string sfo = dir + "/sce_sys/param.sfo";
    std::string titleid, contentid;
    if (!sfo_get(sfo, "TITLE_ID", titleid) || titleid.size() != 9) {
        err = "В vpk нет param.sfo с TITLE_ID";
        return false;
    }
    sfo_get(sfo, "CONTENT_ID", contentid);
    uint8_t h[HEAD_BIN_SIZE];
    memcpy(h, HEAD_BIN_TEMPLATE, HEAD_BIN_SIZE);
    char cid[48];
    memset(cid, 0, sizeof(cid));
    if (contentid.size() >= 36)
        memcpy(cid, contentid.c_str(), std::min(contentid.size(), sizeof(cid) - 1));
    else
        snprintf(cid, sizeof(cid), "EP9000-%s_00-0000000000000000", titleid.c_str());
    memcpy(&h[0x30], cid, 48);
    uint8_t mac[16];
    uint32_t len = be32(&h[0xD0]);
    fpkg_hmac(h, len, mac);
    memcpy(&h[len], mac, 16);
    uint32_t off = be32(&h[0x8]);
    len = be32(&h[0x10]);
    uint32_t out = be32(&h[0xD4]);
    fpkg_hmac(&h[off], len - 64, mac);
    memcpy(&h[out], mac, 16);
    len = be32(&h[0xE8]);
    fpkg_hmac(h, len, mac);
    memcpy(&h[len], mac, 16);
    fs::mkdirs(dir + "/sce_sys/package");
    if (!fs::write(head, h, sizeof(h))) {
        err = "Не удалось записать head.bin";
        return false;
    }
    return true;
}

static bool wait_promoter(std::string& err)
{
    int state = 1;
    while (state) {
        if (scePromoterUtilityGetState(&state) < 0)
            break;
        sceKernelDelayThread(100 * 1000);
    }
    int res = 0;
    if (scePromoterUtilityGetResult(&res) >= 0 && res < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Ошибка установки 0x%08X", (unsigned)res);
        err = buf;
        return false;
    }
    return true;
}

bool promote_app(const std::string& dir, std::string& err)
{
    if (!make_head_bin(dir, err))
        return false;
    std::lock_guard<Mutex> lock(promoter_lock);
    int rc = scePromoterUtilityPromotePkg(dir.c_str(), 0);
    if (rc < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Ошибка установки 0x%08X", (unsigned)rc);
        err = buf;
        return false;
    }
    return wait_promoter(err);
}

bool promote_pkg(const std::string& dir, std::string& err)
{
    std::lock_guard<Mutex> lock(promoter_lock);
    int rc = scePromoterUtilityPromotePkgWithRif(dir.c_str(), 1);
    if (rc < 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Ошибка установки 0x%08X%s", (unsigned)rc,
                 (unsigned)rc == 0x80870004 ? ", проверь NoNpDrm" : "");
        err = buf;
        return false;
    }
    return true;
}
