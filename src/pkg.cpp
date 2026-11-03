#include "pkg.hpp"

#include "promote.hpp"
#include "util.hpp"

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <psp2/io/fcntl.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>

static const uint8_t PKG_VITA_2[16] = {0xe3, 0x1a, 0x70, 0xc9, 0xce, 0x1d, 0xd7, 0x2b, 0xf3, 0xc0, 0x62, 0x29, 0x63, 0xf2, 0xec, 0xcb};
static const uint8_t PKG_VITA_3[16] = {0x42, 0x3a, 0xca, 0x3a, 0x2b, 0xd5, 0x64, 0x9f, 0x96, 0x86, 0xab, 0xad, 0x6f, 0xd8, 0x80, 0x1f};
static const uint8_t PKG_VITA_4[16] = {0xaf, 0x07, 0xfd, 0x59, 0x65, 0x25, 0x27, 0xba, 0xf1, 0x33, 0x89, 0x66, 0x8b, 0x17, 0xd9, 0xea};

static const uint8_t ZRIF_DICT_TAIL[] = {
    48, 48, 48, 48, 57, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 48, 48, 48, 48, 54, 48, 48, 48, 48, 55, 48, 48, 48, 48, 56, 0,
    48, 48, 48, 48, 51, 48, 48, 48, 48, 52, 48, 48, 48, 48, 53, 48, 95, 48, 48, 45, 65, 68, 68, 67, 79, 78, 84, 48, 48, 48,
    48, 50, 45, 80, 67, 83, 71, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 49, 45, 80, 67, 83, 69, 48, 48, 48, 45, 80, 67, 83,
    70, 48, 48, 48, 45, 80, 67, 83, 67, 48, 48, 48, 45, 80, 67, 83, 68, 48, 48, 48, 45, 80, 67, 83, 65, 48, 48, 48, 45, 80,
    67, 83, 66, 48, 48, 48, 0, 1, 0, 1, 0, 1, 0, 2, 239, 205, 171, 137, 103, 69, 35, 1,
};

static const size_t RIF_SIZE = 512;
static const size_t HEADER = 192;
static const size_t HEADER_EXT = 64;

enum { TYPE_PSV_GAME = 21, TYPE_PSV_DLC = 22 };

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t* p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void put64(uint8_t* p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static bool b64(const std::string& in, std::vector<uint8_t>& out)
{
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int d;
        if (c >= 'A' && c <= 'Z')
            d = c - 'A';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            d = c - '0' + 52;
        else if (c == '+' || c == '-')
            d = 62;
        else if (c == '/' || c == '_')
            d = 63;
        else if (c == '=')
            break;
        else
            return false;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return true;
}

bool zrif_decode(const std::string& zrif, std::vector<uint8_t>& rif, std::string& err)
{
    std::vector<uint8_t> raw;
    if (!b64(trim(zrif), raw) || raw.size() < 8) {
        err = "Неверный zRIF";
        return false;
    }
    std::vector<uint8_t> dict(1024 - sizeof(ZRIF_DICT_TAIL), 0);
    dict.insert(dict.end(), ZRIF_DICT_TAIL, ZRIF_DICT_TAIL + sizeof(ZRIF_DICT_TAIL));
    rif.assign(1024, 0);
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK) {
        err = "zlib";
        return false;
    }
    z.next_in = raw.data();
    z.avail_in = raw.size();
    z.next_out = rif.data();
    z.avail_out = rif.size();
    int rc = inflate(&z, Z_FINISH);
    if (rc == Z_NEED_DICT) {
        inflateSetDictionary(&z, dict.data(), dict.size());
        rc = inflate(&z, Z_FINISH);
    }
    size_t n = z.total_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END || n < RIF_SIZE) {
        err = "Не удалось распаковать zRIF";
        return false;
    }
    rif.resize(n);
    return true;
}

namespace {

struct Item {
    std::string name;
    uint64_t offset;
    uint64_t size;
    uint8_t type;
};

class Parser {
public:
    std::string base;
    std::string root;
    std::vector<uint8_t> rif;
    std::string err;
    std::string content_id;
    uint32_t content_type = 0;
    mbedtls_sha256_context sha;

    Parser() { mbedtls_aes_init(&aes); mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0); }
    ~Parser()
    {
        close_file();
        mbedtls_aes_free(&aes);
        mbedtls_sha256_free(&sha);
    }

    bool feed(const uint8_t* d, size_t n)
    {
        mbedtls_sha256_update(&sha, d, n);
        while (n) {
            size_t used = 0;
            if (!step(d, n, used))
                return false;
            d += used;
            n -= used;
            pos += used;
        }
        return true;
    }

    bool finished() const { return head_done && pos == total_size; }

private:
    mbedtls_aes_context aes;
    uint8_t iv[16];
    std::vector<uint8_t> head;
    bool head_done = false;
    uint64_t pos = 0;
    uint64_t target = HEADER + HEADER_EXT;
    int head_stage = 0;
    uint32_t index_count = 0;
    uint64_t total_size = 0;
    uint64_t enc_offset = 0;
    uint64_t enc_size = 0;
    std::vector<Item> items;
    size_t cur = 0;
    SceUID file = -1;
    uint64_t left = 0;
    SceUID tail = -1;

    void ctr(uint64_t offset, uint8_t* buf, size_t len)
    {
        uint8_t counter[16];
        memcpy(counter, iv, 16);
        uint64_t hi = be64(counter);
        uint64_t lo = be64(counter + 8);
        uint64_t nlo = lo + offset / 16;
        if (nlo < lo)
            hi++;
        put64(counter, hi);
        put64(counter + 8, nlo);
        size_t nc = offset % 16;
        uint8_t stream[16];
        if (nc) {
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, counter, stream);
            for (int i = 15; i >= 0; i--)
                if (++counter[i])
                    break;
        }
        mbedtls_aes_crypt_ctr(&aes, len, &nc, counter, stream, buf, buf);
    }

    void close_file()
    {
        if (file >= 0)
            sceIoClose(file);
        file = -1;
        if (tail >= 0)
            sceIoClose(tail);
        tail = -1;
    }

    bool fail(const std::string& e)
    {
        err = e;
        return false;
    }

    bool write_all(SceUID f, const uint8_t* d, size_t n)
    {
        while (n) {
            int w = sceIoWrite(f, d, n);
            if (w <= 0)
                return false;
            d += w;
            n -= w;
        }
        return true;
    }

    bool parse_head()
    {
        if (head_stage == 0) {
            if (be32(head.data()) != 0x7F504B47 || be32(head.data() + HEADER) != 0x7F657874)
                return fail("Это не PKG файл");
            content_id.assign((const char*)head.data() + 0x30, strnlen((const char*)head.data() + 0x30, 0x30));
            if (content_id.size() < 19 || content_id.find('/') != std::string::npos || content_id.find('.') != std::string::npos)
                return fail("PKG повреждён");
            root = base + "/" + content_id;
            if (!rif.empty() && memcmp(rif.data() + 0x10, head.data() + 0x30, 0x30) != 0)
                return fail("zRIF не подходит к этому PKG");
            index_count = be32(head.data() + 20);
            total_size = be64(head.data() + 24);
            enc_offset = be64(head.data() + 32);
            enc_size = be64(head.data() + 40);
            memcpy(iv, head.data() + 0x70, 16);
            int key_type = head[0xe7] & 7;
            const uint8_t* k = key_type == 2 ? PKG_VITA_2 : key_type == 3 ? PKG_VITA_3 : key_type == 4 ? PKG_VITA_4 : nullptr;
            if (!k)
                return fail("Неподдерживаемый тип ключа PKG");
            uint8_t key[16];
            mbedtls_aes_context tmp;
            mbedtls_aes_init(&tmp);
            mbedtls_aes_setkey_enc(&tmp, k, 128);
            mbedtls_aes_crypt_ecb(&tmp, MBEDTLS_AES_ENCRYPT, iv, key);
            mbedtls_aes_free(&tmp);
            mbedtls_aes_setkey_enc(&aes, key, 128);
            target = enc_offset + (uint64_t)index_count * 32;
            if (enc_offset < HEADER + HEADER_EXT || target > 64u * 1024 * 1024 || total_size < enc_offset + enc_size)
                return fail("PKG повреждён");
            head_stage = 1;
            return true;
        }
        if (head_stage == 1) {
            uint32_t meta_offset = be32(head.data() + 8);
            uint32_t meta_count = be32(head.data() + 12);
            uint32_t off = meta_offset;
            for (uint32_t i = 0; i < meta_count; i++) {
                if (off + 16 >= enc_offset)
                    return fail("PKG повреждён");
                uint32_t type = be32(head.data() + off);
                uint32_t size = be32(head.data() + off + 4);
                if (type == 2)
                    content_type = be32(head.data() + off + 8);
                off += 8 + size;
            }
            if (content_type != TYPE_PSV_GAME && content_type != TYPE_PSV_DLC)
                return fail("Поддерживаются только PKG игр, патчей и DLC для Vita");
            uint8_t first[32];
            memcpy(first, head.data() + enc_offset, 32);
            ctr(0, first, 32);
            target = enc_offset + be64(first + 8);
            if (target < head.size() || target > 64u * 1024 * 1024)
                return fail("PKG повреждён");
            head_stage = 2;
            return true;
        }
        for (uint32_t i = 0; i < index_count; i++) {
            uint8_t it[32];
            memcpy(it, head.data() + enc_offset + i * 32, 32);
            ctr(i * 32, it, 32);
            uint32_t name_off = be32(it);
            uint32_t name_len = be32(it + 4);
            Item item;
            item.offset = be64(it + 8);
            item.size = be64(it + 16);
            item.type = it[27];
            if (enc_offset + name_off + name_len > head.size())
                return fail("PKG повреждён");
            std::vector<uint8_t> nm(head.begin() + enc_offset + name_off, head.begin() + enc_offset + name_off + name_len);
            ctr(name_off, nm.data(), nm.size());
            item.name.assign((const char*)nm.data(), nm.size());
            if (item.name.find("..") != std::string::npos)
                return fail("PKG повреждён");
            items.push_back(item);
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.offset < b.offset; });
        fs::rmtree(root);
        fs::mkdirs(root + "/sce_sys/package");
        for (auto& it : items) {
            if (it.type == 4) {
                fs::mkdirs(root + "/" + it.name);
            } else if (it.type != 18 && it.size == 0) {
                fs::mkdirs(fs::parent(root + "/" + it.name));
                fs::write(root + "/" + it.name, "", 0);
            }
        }
        if (!fs::write(root + "/sce_sys/package/head.bin", head.data(), head.size()))
            return fail("Не удалось записать head.bin");
        head_done = true;
        head.clear();
        head.shrink_to_fit();
        return true;
    }

    bool step(const uint8_t* d, size_t n, size_t& used)
    {
        if (!head_done) {
            size_t need = (size_t)(target - head.size());
            used = std::min(n, need);
            head.insert(head.end(), d, d + used);
            if (head.size() == target)
                return parse_head();
            return true;
        }
        uint64_t enc_end = enc_offset + enc_size;
        if (pos >= enc_end) {
            used = (size_t)std::min<uint64_t>(n, total_size > pos ? total_size - pos : n);
            if (tail < 0) {
                std::string p = root + "/sce_sys/package/tail.bin";
                tail = sceIoOpen(p.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
                if (tail < 0)
                    return fail("Не удалось создать tail.bin");
            }
            if (!write_all(tail, d, used))
                return fail("Ошибка записи на карту");
            if (used == 0)
                used = n;
            return true;
        }
        uint64_t rel = pos - enc_offset;
        while (cur < items.size() && (items[cur].type == 4 || items[cur].type == 18 ||
                                      rel >= items[cur].offset + ((items[cur].size + 15) & ~15ull)))
            cur++;
        if (cur >= items.size() || rel < items[cur].offset) {
            uint64_t next = cur < items.size() ? items[cur].offset : enc_size;
            used = (size_t)std::min<uint64_t>(n, next - rel);
            return true;
        }
        Item& it = items[cur];
        uint64_t enc_len = (it.size + 15) & ~15ull;
        if (rel == it.offset) {
            close_file();
            std::string p = root + "/" + it.name;
            fs::mkdirs(fs::parent(p));
            file = sceIoOpen(p.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (file < 0)
                return fail("Не удалось создать " + it.name);
            left = it.size;
        }
        used = (size_t)std::min<uint64_t>(n, it.offset + enc_len - rel);
        buf.assign(d, d + used);
        ctr(rel, buf.data(), used);
        size_t w = (size_t)std::min<uint64_t>(left, used);
        if (file >= 0 && w && !write_all(file, buf.data(), w))
            return fail("Ошибка записи на карту");
        left -= w;
        if (rel + used == it.offset + enc_len) {
            if (file >= 0)
                sceIoClose(file);
            file = -1;
            cur++;
        }
        return true;
    }

    std::vector<uint8_t> buf;
};

}

static std::string hex(const uint8_t* d, size_t n)
{
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += h[d[i] >> 4];
        s += h[d[i] & 15];
    }
    return s;
}

bool pkg_install(const std::string& url, const std::string& zrif, const std::string& sha256,
                 const std::string& workdir, net::Progress* progress, std::string& stage, std::string& err)
{
    Parser p;
    if (!zrif.empty() && !zrif_decode(zrif, p.rif, err))
        return false;
    p.base = workdir;
    stage = "Загрузка и распаковка";
    bool ok = net::stream(url, [&](const uint8_t* d, size_t n) { return p.feed(d, n); }, progress, err);
    if (!ok) {
        if (!p.err.empty())
            err = p.err;
        if (!p.root.empty())
            fs::rmtree(p.root);
        return false;
    }
    if (!p.finished()) {
        err = "PKG скачан не полностью";
        if (!p.root.empty())
            fs::rmtree(p.root);
        return false;
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&p.sha, digest);
    if (!sha256.empty() && lower(sha256) != hex(digest, 32)) {
        err = "Контрольная сумма PKG не совпала";
        fs::rmtree(p.root);
        return false;
    }
    std::vector<uint8_t> stat(768, 0);
    fs::write(p.root + "/sce_sys/package/stat.bin", stat.data(), stat.size());
    if (!p.rif.empty())
        fs::write(p.root + "/sce_sys/package/work.bin", p.rif.data(), RIF_SIZE);
    stage = "Установка";
    bool res = promote_pkg(p.root, err);
    fs::rmtree(p.root);
    return res;
}
