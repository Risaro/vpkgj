#include "zip.hpp"

#include "util.hpp"

#include <minizip/unzip.h>
#include <psp2/io/fcntl.h>

#include <cstring>
#include <vector>

static bool is_zip_file(const std::string& path)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0)
        return false;
    unsigned char m[4] = {0};
    sceIoRead(f, m, 4);
    sceIoClose(f);
    return m[0] == 'P' && m[1] == 'K' && (m[2] == 3 || m[2] == 5 || m[2] == 7);
}

static uint64_t tar_octal(const char* p, int n)
{
    uint64_t v = 0;
    while (n > 0 && (*p == ' ' || *p == '\0')) {
        p++;
        n--;
    }
    for (; n > 0 && *p >= '0' && *p <= '7'; p++, n--)
        v = (v << 3) | (*p - '0');
    return v;
}

static bool tar_extract(const std::string& path, const std::string& dest, net::Progress* p, std::string& err,
                        const ZipFilter& map)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0) {
        err = "Не удалось открыть архив";
        return false;
    }
    SceOff fsize = sceIoLseek(f, 0, SCE_SEEK_END);
    sceIoLseek(f, 0, SCE_SEEK_SET);
    if (p) {
        p->total = fsize > 0 ? (uint64_t)fsize : 0;
        p->done = 0;
    }
    std::vector<uint8_t> buf(256 * 1024);
    char hdr[512];
    std::string longname;
    uint64_t consumed = 0;
    int zero_blocks = 0;
    bool ok = true;
    for (;;) {
        int r = sceIoRead(f, hdr, 512);
        if (r == 0)
            break;
        if (r != 512) {
            err = "Архив повреждён (обрезан)";
            ok = false;
            break;
        }
        consumed += 512;
        bool allzero = true;
        for (int i = 0; i < 512; i++)
            if (hdr[i]) {
                allzero = false;
                break;
            }
        if (allzero) {
            if (++zero_blocks >= 2)
                break;
            continue;
        }
        zero_blocks = 0;
        char typeflag = hdr[156];
        uint64_t size = tar_octal(hdr + 124, 12);
        uint64_t padded = (size + 511) & ~((uint64_t)511);

        if (typeflag == 'L' || typeflag == 'x' || typeflag == 'g') {
            std::string ext;
            ext.resize(size);
            uint64_t got = 0;
            while (got < size) {
                int want = (int)std::min<uint64_t>(buf.size(), size - got);
                int rd = sceIoRead(f, buf.data(), want);
                if (rd <= 0) {
                    ok = false;
                    break;
                }
                memcpy(&ext[got], buf.data(), rd);
                got += rd;
            }
            if (padded > size)
                sceIoLseek(f, padded - size, SCE_SEEK_CUR);
            consumed += padded;
            if (p)
                p->done = consumed;
            if (!ok) {
                err = "Ошибка чтения заголовка";
                break;
            }
            if (typeflag == 'L') {
                longname = ext.c_str();
            } else {
                size_t pos = ext.find(" path=");
                if (pos != std::string::npos) {
                    size_t s = pos + 6, e = ext.find('\n', s);
                    if (e != std::string::npos)
                        longname = ext.substr(s, e - s);
                }
            }
            continue;
        }

        std::string name = longname;
        longname.clear();
        if (name.empty()) {
            char nm[256];
            size_t pl = strnlen(hdr + 345, 155);
            size_t nl = strnlen(hdr, 100);
            if (pl) {
                memcpy(nm, hdr + 345, pl);
                nm[pl] = '/';
                memcpy(nm + pl + 1, hdr, nl);
                name.assign(nm, pl + 1 + nl);
            } else {
                name.assign(hdr, nl);
            }
        }
        for (auto& c : name)
            if (c == '\\')
                c = '/';
        while (name.size() >= 2 && name[0] == '.' && name[1] == '/')
            name.erase(0, 2);

        bool isdir = (typeflag == '5') || (!name.empty() && name.back() == '/');
        if (typeflag != '0' && typeflag != '\0' && typeflag != '5') {
            if (padded)
                sceIoLseek(f, padded, SCE_SEEK_CUR);
            consumed += padded;
            if (p)
                p->done = consumed;
            continue;
        }

        if (p && p->cancel) {
            err = "Отменено";
            ok = false;
            break;
        }
        if (name.empty() || name.find("../") != std::string::npos || starts_with(name, "/")) {
            if (padded)
                sceIoLseek(f, padded, SCE_SEEK_CUR);
            consumed += padded;
            continue;
        }
        std::string rel = map ? map(isdir ? name : name) : name;
        if (rel.empty()) {
            if (padded)
                sceIoLseek(f, padded, SCE_SEEK_CUR);
            consumed += padded;
            if (p)
                p->done = consumed;
            continue;
        }
        std::string out = rel.find(':') != std::string::npos ? rel : dest + "/" + rel;
        if (isdir) {
            fs::mkdirs(out.size() && out.back() == '/' ? out.substr(0, out.size() - 1) : out);
            consumed += padded;
            if (p)
                p->done = consumed;
            continue;
        }
        fs::mkdirs(fs::parent(out));
        SceUID o = sceIoOpen(out.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (o < 0) {
            err = "Не удалось создать " + out;
            ok = false;
            break;
        }
        uint64_t left = size;
        while (left > 0) {
            int want = (int)std::min<uint64_t>(buf.size(), left);
            int rd = sceIoRead(f, buf.data(), want);
            if (rd <= 0) {
                err = "Архив повреждён: " + name;
                ok = false;
                break;
            }
            int pos = 0;
            while (pos < rd) {
                int w = sceIoWrite(o, buf.data() + pos, rd - pos);
                if (w <= 0) {
                    err = "Ошибка записи, проверь место на карте";
                    ok = false;
                    break;
                }
                pos += w;
            }
            if (!ok)
                break;
            left -= rd;
            consumed += rd;
            if (p)
                p->done = consumed;
        }
        sceIoClose(o);
        if (!ok)
            break;
        uint64_t pad = padded - size;
        if (pad) {
            sceIoLseek(f, pad, SCE_SEEK_CUR);
            consumed += pad;
        }
        if (p)
            p->done = consumed;
    }
    sceIoClose(f);
    return ok;
}

static bool each(const std::string& path, std::string& err,
                 const std::function<bool(unzFile, const std::string&, const unz_file_info64&)>& fn)
{
    unzFile z = unzOpen64(path.c_str());
    if (!z) {
        err = "Архив повреждён или это не zip";
        return false;
    }
    bool ok = true;
    int rc = unzGoToFirstFile(z);
    while (rc == UNZ_OK) {
        unz_file_info64 info;
        char name[1024];
        if (unzGetCurrentFileInfo64(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) {
            err = "Ошибка чтения архива";
            ok = false;
            break;
        }
        if (!fn(z, name, info)) {
            ok = false;
            break;
        }
        rc = unzGoToNextFile(z);
    }
    unzClose(z);
    return ok;
}

static bool tar_list(const std::string& path, std::vector<std::string>& names, std::string& err)
{
    SceUID f = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (f < 0) {
        err = "Не удалось открыть архив";
        return false;
    }
    char hdr[512];
    std::string longname;
    int zero_blocks = 0;
    for (;;) {
        int r = sceIoRead(f, hdr, 512);
        if (r == 0)
            break;
        if (r != 512)
            break;
        bool allzero = true;
        for (int i = 0; i < 512; i++)
            if (hdr[i]) {
                allzero = false;
                break;
            }
        if (allzero) {
            if (++zero_blocks >= 2)
                break;
            continue;
        }
        zero_blocks = 0;
        char typeflag = hdr[156];
        uint64_t size = tar_octal(hdr + 124, 12);
        uint64_t padded = (size + 511) & ~((uint64_t)511);
        if (typeflag == 'L' || typeflag == 'x' || typeflag == 'g') {
            std::string ext;
            ext.resize(size);
            sceIoRead(f, &ext[0], size);
            if (padded > size)
                sceIoLseek(f, padded - size, SCE_SEEK_CUR);
            if (typeflag == 'L')
                longname = ext.c_str();
            else {
                size_t pos = ext.find(" path=");
                if (pos != std::string::npos) {
                    size_t s = pos + 6, e = ext.find('\n', s);
                    if (e != std::string::npos)
                        longname = ext.substr(s, e - s);
                }
            }
            continue;
        }
        std::string name = longname;
        longname.clear();
        if (name.empty()) {
            size_t pl = strnlen(hdr + 345, 155), nl = strnlen(hdr, 100);
            if (pl) {
                name.assign(hdr + 345, pl);
                name += '/';
                name.append(hdr, nl);
            } else {
                name.assign(hdr, nl);
            }
        }
        while (name.size() >= 2 && name[0] == '.' && name[1] == '/')
            name.erase(0, 2);
        if (!name.empty())
            names.push_back(name);
        if (padded)
            sceIoLseek(f, padded, SCE_SEEK_CUR);
    }
    sceIoClose(f);
    return true;
}

bool zip_list(const std::string& path, std::vector<std::string>& names, std::string& err)
{
    if (!is_zip_file(path))
        return tar_list(path, names, err);
    return each(path, err, [&](unzFile, const std::string& n, const unz_file_info64&) {
        names.push_back(n);
        return true;
    });
}

bool zip_extract(const std::string& path, const std::string& dest, net::Progress* p, std::string& err,
                 const ZipFilter& map)
{
    if (!is_zip_file(path))
        return tar_extract(path, dest, p, err, map);
    uint64_t total = 0;
    if (!each(path, err, [&](unzFile, const std::string&, const unz_file_info64& i) {
            total += i.uncompressed_size;
            return true;
        }))
        return false;
    if (p) {
        p->total = total;
        p->done = 0;
    }
    std::vector<uint8_t> buf(256 * 1024);
    uint64_t done = 0;
    return each(path, err, [&](unzFile z, const std::string& raw, const unz_file_info64& info) {
        if (p && p->cancel) {
            err = "Отменено";
            return false;
        }
        std::string name = raw;
        for (auto& c : name)
            if (c == '\\')
                c = '/';
        if (name.find("../") != std::string::npos || starts_with(name, "/"))
            return true;
        std::string rel = map ? map(name) : name;
        if (rel.empty())
            return true;
        std::string out = rel.find(':') != std::string::npos ? rel : dest + "/" + rel;
        if (ends_with(name, "/")) {
            fs::mkdirs(out.substr(0, out.size() - 1));
            return true;
        }
        fs::mkdirs(fs::parent(out));
        if (unzOpenCurrentFile(z) != UNZ_OK) {
            err = "Не удалось распаковать " + name;
            return false;
        }
        SceUID f = sceIoOpen(out.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (f < 0) {
            unzCloseCurrentFile(z);
            err = "Не удалось создать " + out;
            return false;
        }
        bool ok = true;
        for (;;) {
            int r = unzReadCurrentFile(z, buf.data(), buf.size());
            if (r < 0) {
                err = "Архив повреждён: " + name;
                ok = false;
                break;
            }
            if (r == 0)
                break;
            int pos = 0;
            while (pos < r) {
                int w = sceIoWrite(f, buf.data() + pos, r - pos);
                if (w <= 0) {
                    err = "Ошибка записи, проверь место на карте";
                    ok = false;
                    break;
                }
                pos += w;
            }
            if (!ok)
                break;
            done += r;
            if (p)
                p->done = done;
        }
        sceIoClose(f);
        unzCloseCurrentFile(z);
        return ok;
    });
}
