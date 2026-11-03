#include "zip.hpp"

#include "util.hpp"

#include <minizip/unzip.h>
#include <psp2/io/fcntl.h>

#include <vector>

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

bool zip_list(const std::string& path, std::vector<std::string>& names, std::string& err)
{
    return each(path, err, [&](unzFile, const std::string& n, const unz_file_info64&) {
        names.push_back(n);
        return true;
    });
}

bool zip_extract(const std::string& path, const std::string& dest, net::Progress* p, std::string& err,
                 const ZipFilter& map)
{
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
