#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <taihen.h>
#include <taipool.h>

#define DIR "ux0:data/VPKGJ"
#define INDEX_MAX (768 * 1024)
#define CHUNK (128 * 1024)
#define QUEUE_MAX (16 * 1024)
#define NET_SIZE (141 * 1024)
#define APP_ID "VPKG00001"

const int sceKernelPreloadModuleInhibit = SCE_KERNEL_PRELOAD_INHIBIT_LIBDBG;

static int (*notice_init)(void* data);
static int (*set_utf8)(void* data, const char* text, SceSize len);
static int (*notice_clean)(void* data);
static int (*lsdb_send)(void* data, int a2);

static char* index_buf;
static char* chunk;
static char* line;
static char* installed;
static char* versions;
static char* notified;
static char* fresh;
static char* queue_buf;
static char* bgq;
static char* bgadd;
static int fresh_len;
static int found;
static char first_name[96];

static int module_offset(SceUID modid, int seg, uint32_t off, void* out)
{
    SceKernelModuleInfo info;
    info.size = sizeof(info);
    if (sceKernelGetModuleInfo(modid, &info) < 0 || off > info.segments[seg].memsz)
        return -1;
    *(uint32_t*)out = (uint32_t)info.segments[seg].vaddr + off;
    return 0;
}

static int slen(const char* s)
{
    return sceClibStrnlen(s, 0x100000);
}

static int read_file(const char* path, char* buf, int max)
{
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) {
        buf[0] = 0;
        return 0;
    }
    int n = sceIoRead(f, buf, max - 1);
    sceIoClose(f);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    return n;
}

static void append_file(const char* path, const char* text, int len)
{
    SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (f >= 0) {
        sceIoWrite(f, text, len);
        sceIoClose(f);
    }
}

static int config_value(const char* text, const char* key, char* out, int max)
{
    int kl = slen(key);
    const char* p = text;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!sceClibStrncmp(p, key, kl) && (p[kl] == '=' || p[kl] == ' ' || p[kl] == '\t')) {
            p += kl;
            while (*p == '=' || *p == ' ' || *p == '\t')
                p++;
            int i = 0;
            while (*p && *p != '\r' && *p != '\n' && *p != ' ' && i < max - 1)
                out[i++] = *p++;
            out[i] = 0;
            return i;
        }
        while (*p && *p != '\n')
            p++;
    }
    out[0] = 0;
    return 0;
}

static int version_cmp(const char* a, const char* b)
{
    while (*a || *b) {
        long x = 0, y = 0;
        while (*a && (*a < '0' || *a > '9'))
            a++;
        while (*a >= '0' && *a <= '9')
            x = x * 10 + (*a++ - '0');
        while (*b && (*b < '0' || *b > '9'))
            b++;
        while (*b >= '0' && *b <= '9')
            y = y * 10 + (*b++ - '0');
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

typedef int (*Sink)(const char* data, int len);

static int http_get(const char* url, Sink sink)
{
    int ok = 0;
    int tpl = sceHttpCreateTemplate("VPKGJ-daemon/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0)
        return 0;
    sceHttpSetConnectTimeOut(tpl, 20 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 60 * 1000 * 1000);
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
        if (req >= 0) {
            int status = 0;
            if (sceHttpSendRequest(req, 0, 0) >= 0 && sceHttpGetStatusCode(req, &status) >= 0 && status == 200) {
                ok = 1;
                for (;;) {
                    int n = sceHttpReadData(req, chunk, CHUNK);
                    if (n < 0)
                        ok = 0;
                    if (n <= 0)
                        break;
                    if (!sink(chunk, n)) {
                        ok = 0;
                        break;
                    }
                }
            }
            sceHttpDeleteRequest(req);
        }
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl);
    return ok;
}

static int index_len;

static int index_sink(const char* d, int n)
{
    if (index_len + n >= INDEX_MAX)
        return 0;
    sceClibMemcpy(index_buf + index_len, d, n);
    index_len += n;
    index_buf[index_len] = 0;
    return 1;
}

static const char* json_str(const char* from, const char* end, const char* key, char* out, int max)
{
    char pat[48];
    sceClibSnprintf(pat, sizeof(pat), "\"%s\": \"", key);
    const char* p = sceClibStrstr(from, pat);
    out[0] = 0;
    if (!p || (end && p > end))
        return 0;
    p += slen(pat);
    int i = 0;
    while (*p && *p != '"' && i < max - 1)
        out[i++] = *p++;
    out[i] = 0;
    return p;
}

static void remember(const char* key, const char* name)
{
    if (sceClibStrstr(notified, key))
        return;
    int kl = slen(key);
    if (fresh_len + kl + 1 < 16 * 1024) {
        sceClibMemcpy(fresh + fresh_len, key, kl);
        fresh_len += kl;
        fresh[fresh_len] = 0;
    }
    if (!found)
        sceClibSnprintf(first_name, sizeof(first_name), "%s", name);
    found++;
}

static void check_catalog(void)
{
    const char* p = installed;
    while (*p) {
        char key[64], ver[32];
        int i = 0;
        while (*p && *p != '\t' && *p != '\n' && i < 63)
            key[i++] = *p++;
        key[i] = 0;
        i = 0;
        if (*p == '\t')
            p++;
        while (*p && *p != '\n' && *p != '\r' && i < 31)
            ver[i++] = *p++;
        ver[i] = 0;
        while (*p == '\r' || *p == '\n')
            p++;
        char* slash = sceClibStrstr(key, "/");
        if (!slash || sceClibStrncmp(key, "games/", 6) == 0)
            continue;
        char pat[64];
        const char* id = slash + 1;
        int plugins = !sceClibStrncmp(key, "plugins/", 8);
        sceClibSnprintf(pat, sizeof(pat), plugins ? "\"id\": \"%s\"" : "\"titleid\": \"%s\"", id);
        const char* obj = sceClibStrstr(index_buf, pat);
        if (!obj)
            continue;
        const char* end = sceClibStrstr(obj, "}");
        const char* start = obj;
        while (start > index_buf && *start != '{')
            start--;
        char nver[32], name[96];
        json_str(start, end, "version", nver, sizeof(nver));
        json_str(start, end, "name", name, sizeof(name));
        if (!nver[0] || !sceClibStrncmp(nver, ver, 32))
            continue;
        char nk[128];
        sceClibSnprintf(nk, sizeof(nk), "%s/%s\n", key, nver);
        remember(nk, name);
    }
}

static int carry;

static void patch_line(char* l)
{
    char* f[9];
    int n = 0;
    f[n++] = l;
    for (char* c = l; *c && n < 9; c++) {
        if (*c == '\t') {
            *c = 0;
            f[n++] = c + 1;
        }
    }
    if (n < 4 || slen(f[0]) != 9)
        return;
    char pat[16];
    sceClibSnprintf(pat, sizeof(pat), "%s\t", f[0]);
    char* v = sceClibStrstr(versions, pat);
    if (!v)
        return;
    v += 10;
    char cur[16];
    int i = 0;
    while (v[i] && v[i] != '\n' && v[i] != '\r' && i < 15) {
        cur[i] = v[i];
        i++;
    }
    cur[i] = 0;
    if (version_cmp(cur, f[3]) >= 0)
        return;
    char nk[64];
    sceClibSnprintf(nk, sizeof(nk), "games/%s/%s\n", f[0], f[3]);
    remember(nk, f[2]);
}

static int patches_sink(const char* d, int n)
{
    for (int i = 0; i < n; i++) {
        char c = d[i];
        if (c == '\n') {
            line[carry] = 0;
            if (carry && line[carry - 1] == '\r')
                line[carry - 1] = 0;
            patch_line(line);
            carry = 0;
        } else if (carry < 2047) {
            line[carry++] = c;
        }
    }
    return 1;
}

static void notify(const char* text)
{
    if (!notice_init || !set_utf8 || !notice_clean || !lsdb_send)
        return;
    char data[0x100];
    notice_init(data);
    set_utf8(&data[0x0], APP_ID, 9);
    set_utf8(&data[0xC], "LAUPDATE", 8);
    *(uint32_t*)(&data[0x28]) = 0x02;
    data[0x2C] = 0x01;
    const char* icon = "ux0:app/" APP_ID "/sce_sys/icon0.png";
    set_utf8(&data[0x30], icon, slen(icon));
    set_utf8(&data[0xBC], text, slen(text));
    *(uint32_t*)(&data[0xC8]) = 0x20000;
    set_utf8(&data[0xCC], APP_ID, 9);
    set_utf8(&data[0xD8], "updates", 7);
    lsdb_send(data, 1);
    notice_clean(data);
}

static void check(void)
{
    char cfg[512], repo[128], branch[64], url[320];
    read_file(DIR "/config.txt", cfg, sizeof(cfg));
    if (!config_value(cfg, "repo", repo, sizeof(repo)))
        sceClibSnprintf(repo, sizeof(repo), "Risaro/vpkgj-db");
    if (!config_value(cfg, "branch", branch, sizeof(branch)))
        sceClibSnprintf(branch, sizeof(branch), "main");
    read_file(DIR "/installed.txt", installed, 32 * 1024);
    read_file(DIR "/game_versions.txt", versions, 32 * 1024);
    read_file(DIR "/notified.txt", notified, 64 * 1024);
    fresh_len = 0;
    fresh[0] = 0;
    found = 0;
    first_name[0] = 0;
    if (installed[0]) {
        sceClibSnprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/index.json", repo, branch);
        index_len = 0;
        index_buf[0] = 0;
        if (http_get(url, index_sink))
            check_catalog();
    }
    if (versions[0]) {
        char pkgj[2048];
        if (!read_file("ux0:pkgj/config.txt", pkgj, sizeof(pkgj)))
            read_file("ur0:pkgj/config.txt", pkgj, sizeof(pkgj));
        if (config_value(pkgj, "url_updates", url, sizeof(url))) {
            carry = 0;
            http_get(url, patches_sink);
        }
    }
    if (!found)
        return;
    append_file(DIR "/notified.txt", fresh, fresh_len);
    char text[160];
    if (found == 1)
        sceClibSnprintf(text, sizeof(text), "Вышло обновление: %s", first_name);
    else
        sceClibSnprintf(text, sizeof(text), "Доступно обновлений: %d. Открой VPKGJ, вкладка Загрузки", found);
    notify(text);
}


static int online(void)
{
    int state = 0;
    return sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

static void hash_name(const char* s, char* out)
{
    unsigned long long h = 1469598103934665603ull;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ull;
    }
    static const char* hex = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        out[i] = hex[h & 15];
        h >>= 4;
    }
    out[16] = 0;
}

static int exists(const char* path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static long long file_size(const char* path)
{
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0)
        return -1;
    return st.st_size;
}

static void write_file(const char* path, const char* text, int len)
{
    SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (f >= 0) {
        if (len > 0)
            sceIoWrite(f, text, len);
        sceIoClose(f);
    }
}

static void drain_queue(void);

static char cur_key[128];
static char cur_name[96];
static unsigned long long state_at;

static void save_state(unsigned long long done, unsigned long long total)
{
    char text[320];
    int n = sceClibSnprintf(text, sizeof(text), "%s\t%s\t%u\t%u\n", cur_key, cur_name, (unsigned)(done / 1024), (unsigned)(total / 1024));
    write_file(DIR "/bgstate.txt", text, n);
}

static int stop_requested(void)
{
    return exists(DIR "/bgq.stop") || exists(DIR "/daemon_off");
}

static int download(const char* url, const char* path)
{
    char part[200];
    sceClibSnprintf(part, sizeof(part), "%s.part", path);
    long long have = file_size(part);
    if (have < 0)
        have = 0;
    int ok = 0;
    int tpl = sceHttpCreateTemplate("VPKGJ-daemon/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0)
        return 0;
    sceHttpSetConnectTimeOut(tpl, 20 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 60 * 1000 * 1000);
    sceHttpSetAutoRedirect(tpl, 1);
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
        if (req >= 0) {
            if (have > 0) {
                char range[48];
                sceClibSnprintf(range, sizeof(range), "bytes=%u-", (unsigned)have);
                sceHttpAddRequestHeader(req, "Range", range, SCE_HTTP_HEADER_ADD);
            }
            int status = 0;
            if (sceHttpSendRequest(req, 0, 0) >= 0 && sceHttpGetStatusCode(req, &status) >= 0 && (status == 200 || status == 206)) {
                unsigned long long len = 0;
                if (sceHttpGetResponseContentLength(req, &len) < 0)
                    len = 0;
                if (status == 200)
                    have = 0;
                unsigned long long total = len ? have + len : 0;
                SceUID f = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | (have ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
                if (f >= 0) {
                    unsigned long long done = have;
                    ok = 1;
                    for (;;) {
                        int n = sceHttpReadData(req, chunk, CHUNK);
                        if (n < 0)
                            ok = 0;
                        if (n <= 0)
                            break;
                        if (sceIoWrite(f, chunk, n) != n) {
                            ok = 0;
                            break;
                        }
                        done += n;
                        if (done - state_at >= 2 * 1024 * 1024) {
                            state_at = done;
                            save_state(done, total);
                            drain_queue();
                            if (stop_requested()) {
                                ok = 0;
                                break;
                            }
                        }
                    }
                    sceIoClose(f);
                    if (ok && total && done < total)
                        ok = 0;
                }
            } else if (status == 416) {
                sceIoRemove(part);
            }
            sceHttpDeleteRequest(req);
        }
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl);
    if (ok) {
        sceIoRemove(path);
        ok = sceIoRename(part, path) >= 0;
    }
    return ok;
}

static void merge_added(void)
{
    if (!exists(DIR "/bgq.add"))
        return;
    sceIoRemove(DIR "/bgq.in");
    if (sceIoRename(DIR "/bgq.add", DIR "/bgq.in") < 0)
        return;
    int n = read_file(DIR "/bgq.in", bgadd, QUEUE_MAX);
    sceIoRemove(DIR "/bgq.in");
    if (n > 0)
        append_file(DIR "/bgq.txt", bgadd, n);
}

static int fails;
static unsigned long long retry_at;

static unsigned long long now_s(void)
{
    return sceKernelGetSystemTimeWide() / 1000000ull;
}

static void field(const char** p, char* out, int max)
{
    int i = 0;
    while (**p && **p != '\t' && **p != '\n' && **p != '\r') {
        if (i < max - 1)
            out[i++] = **p;
        (*p)++;
    }
    out[i] = 0;
    if (**p == '\t')
        (*p)++;
}

static void drop_first(int keep_key_done)
{
    int n = read_file(DIR "/bgq.txt", bgq, QUEUE_MAX);
    char* nl = bgq;
    while (*nl && *nl != '\n')
        nl++;
    if (*nl)
        nl++;
    int rest = n - (int)(nl - bgq);
    if (rest <= 0)
        sceIoRemove(DIR "/bgq.txt");
    else
        write_file(DIR "/bgq.txt", nl, rest);
    if (!keep_key_done)
        return;
    char pat[140];
    sceClibSnprintf(pat, sizeof(pat), "%s\t", cur_key);
    if (rest > 0 && sceClibStrstr(nl, pat))
        return;
    char line[160];
    int l = sceClibSnprintf(line, sizeof(line), "%s\n", cur_key);
    append_file(DIR "/bgdone.txt", line, l);
    char text[200];
    sceClibSnprintf(text, sizeof(text), "Скачано в фоне: %s. Открой VPKGJ, чтобы установить", cur_name);
    notify(text);
}

static int process_queue(void)
{
    if (exists(DIR "/bgq.stop")) {
        sceIoRemove(DIR "/bgq.txt");
        sceIoRemove(DIR "/bgq.add");
        sceIoRemove(DIR "/bgq.stop");
        sceIoRemove(DIR "/bgstate.txt");
        fails = 0;
        return 0;
    }
    merge_added();
    if (read_file(DIR "/bgq.txt", bgq, QUEUE_MAX) <= 0) {
        sceIoRemove(DIR "/bgstate.txt");
        return 0;
    }
    if (now_s() < retry_at || !online())
        return 0;
    const char* p = bgq;
    char url[1024], hash[17], path[160];
    field(&p, cur_key, sizeof(cur_key));
    field(&p, cur_name, sizeof(cur_name));
    field(&p, url, sizeof(url));
    if (!cur_key[0] || !url[0]) {
        drop_first(0);
        return 1;
    }
    hash_name(url, hash);
    sceClibSnprintf(path, sizeof(path), DIR "/bg/%s", hash);
    sceIoMkdir(DIR "/bg", 0777);
    state_at = 0;
    long long have = file_size(path);
    if (have < 0) {
        save_state(0, 0);
        if (!download(url, path)) {
            if (stop_requested())
                return 0;
            fails++;
            if (fails >= 6) {
                char text[200];
                sceClibSnprintf(text, sizeof(text), "Не удалось скачать в фоне: %s", cur_name);
                notify(text);
                drop_first(0);
                fails = 0;
            } else {
                retry_at = now_s() + fails * 30;
            }
            return 0;
        }
    }
    fails = 0;
    drop_first(1);
    sceIoRemove(DIR "/bgstate.txt");
    return 1;
}

static void drain_queue(void)
{
    SceIoStat st;
    if (sceIoGetstat(DIR "/notify.txt", &st) < 0)
        return;
    sceIoRemove(DIR "/notify.work");
    if (sceIoRename(DIR "/notify.txt", DIR "/notify.work") < 0)
        return;
    read_file(DIR "/notify.work", queue_buf, 4096);
    sceIoRemove(DIR "/notify.work");
    char* p = queue_buf;
    while (*p) {
        char* e = p;
        while (*e && *e != '\n')
            e++;
        char c = *e;
        *e = 0;
        if (e > p && e[-1] == '\r')
            e[-1] = 0;
        if (*p) {
            notify(p);
            sceKernelDelayThread(1500 * 1000);
        }
        if (!c)
            break;
        p = e + 1;
    }
}

static int daemon_thread(SceSize args, void* argp)
{
    taiGetModuleExportFunc("SceLsdb", 0xFFFFFFFF, 0x315B9FD6, (uintptr_t*)&lsdb_send);
    sceKernelDelayThread(20 * 1000 * 1000);
    for (int i = 0; i < 14; i++) {
        drain_queue();
        sceKernelDelayThread(5 * 1000 * 1000);
    }
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    if (sceNetShowNetstat() == SCE_NET_ERROR_ENOTINIT) {
        SceNetInitParam p;
        p.memory = taipool_alloc(NET_SIZE);
        p.size = NET_SIZE;
        p.flags = 0;
        sceNetInit(&p);
    }
    sceNetCtlInit();
    sceHttpInit(0x40000);
    sceSslInit(0x40000);
    sceHttpsDisableOption(SCE_HTTPS_FLAG_SERVER_VERIFY);
    unsigned long long next_check = 0;
    for (;;) {
        SceIoStat st;
        int off = sceIoGetstat(DIR "/daemon_off", &st) >= 0;
        if (!off && now_s() >= next_check && online()) {
            check();
            next_check = now_s() + 6 * 3600;
        }
        if (!off) {
            drain_queue();
            if (process_queue()) {
                sceKernelDelayThread(300 * 1000);
                continue;
            }
        } else {
            sceIoRemove(DIR "/notify.txt");
        }
        sceKernelDelayThread(5 * 1000 * 1000);
    }
    return 0;
}

int module_start(SceSize argc, const void* args)
{
    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfo("SceShell", &info) < 0)
        return SCE_KERNEL_START_FAILED;
    switch (info.module_nid) {
    case 0x0552F692:
        module_offset(info.modid, 0, 0x42930C | 1, &notice_init);
        module_offset(info.modid, 0, 0x408E14 | 1, &set_utf8);
        module_offset(info.modid, 0, 0x4163E8 | 1, &notice_clean);
        break;
    case 0x6CB01295:
        module_offset(info.modid, 0, 0x41AA30 | 1, &notice_init);
        module_offset(info.modid, 0, 0x3FAD88 | 1, &set_utf8);
        module_offset(info.modid, 0, 0x408298 | 1, &notice_clean);
        break;
    case 0x5549BF1F:
        module_offset(info.modid, 0, 0x429754 | 1, &notice_init);
        module_offset(info.modid, 0, 0x40925C | 1, &set_utf8);
        module_offset(info.modid, 0, 0x416830 | 1, &notice_clean);
        break;
    default:
        return SCE_KERNEL_START_FAILED;
    }
    taipool_init(INDEX_MAX + CHUNK + 2048 + 32 * 1024 * 2 + 64 * 1024 + 16 * 1024 + 4096 + QUEUE_MAX * 2 + NET_SIZE + 64 * 1024);
    index_buf = taipool_alloc(INDEX_MAX);
    chunk = taipool_alloc(CHUNK);
    line = taipool_alloc(2048);
    installed = taipool_alloc(32 * 1024);
    versions = taipool_alloc(32 * 1024);
    notified = taipool_alloc(64 * 1024);
    fresh = taipool_alloc(16 * 1024);
    queue_buf = taipool_alloc(4096);
    bgq = taipool_alloc(QUEUE_MAX);
    bgadd = taipool_alloc(QUEUE_MAX);
    if (!bgq || !bgadd || !queue_buf || !index_buf || !chunk || !line || !installed || !versions || !notified || !fresh)
        return SCE_KERNEL_START_FAILED;
    SceUID th = sceKernelCreateThread("vpkgj_daemon", daemon_thread, 0xB0, 0x10000, 0, 0, 0);
    if (th >= 0)
        sceKernelStartThread(th, 0, 0);
    return SCE_KERNEL_START_SUCCESS;
}

int _start(SceSize argc, const void* args) __attribute__((weak, alias("module_start")));

int module_stop(SceSize argc, const void* args)
{
    return SCE_KERNEL_STOP_SUCCESS;
}
