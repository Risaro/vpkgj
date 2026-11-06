#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <taihen.h>
#include <taipool.h>

#include "dnet.h"

static void dlog(const char* fmt, ...);
#include <stdarg.h>

#define DIR "ux0:data/VPKGJ"
#define CHUNK (32 * 1024)
#define QUEUE_MAX (16 * 1024)
#define NET_SIZE (141 * 1024)
#define APP_ID "VPKG00001"
#define TLS_POOL (96 * 1024)

const int sceKernelPreloadModuleInhibit = SCE_KERNEL_PRELOAD_INHIBIT_LIBDBG;

static int (*notice_init)(void* data);
static int (*set_utf8)(void* data, const char* text, SceSize len);
static int (*notice_clean)(void* data);
static int (*lsdb_send)(void* data, int a2);

static unsigned int shell_nid;
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
        sceIoSyncByFd(f, 0);
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
    int ok = dnet_get(url, 0, 0, sink);
    if (!ok)
        dlog("get %s: http %d err 0x%08X", url, dnet_status, (unsigned)dnet_err);
    return ok;
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

static char ix_cat[24];
static char ix_id[64];
static char ix_ver[32];
static char ix_name[96];

static void copy_value(const char* p, char* out, int max)
{
    int i = 0;
    while (*p && *p != '"' && i < max - 1)
        out[i++] = *p++;
    out[i] = 0;
}

static void item_done(void)
{
    if (!ix_id[0] || !ix_ver[0] || !ix_cat[0] || !sceClibStrcmp(ix_cat, "games"))
        return;
    char pat[96];
    int pl = sceClibSnprintf(pat, sizeof(pat), "%s/%s\t", ix_cat, ix_id);
    const char* p = installed;
    while ((p = sceClibStrstr(p, pat)) && p != installed && p[-1] != '\n')
        p += pl;
    if (!p)
        return;
    p += pl;
    char ver[32];
    int i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '\r' && i < 31) {
        ver[i] = p[i];
        i++;
    }
    ver[i] = 0;
    if (!sceClibStrncmp(ix_ver, ver, 32))
        return;
    char nk[160];
    sceClibSnprintf(nk, sizeof(nk), "%s/%s/%s\n", ix_cat, ix_id, ix_ver);
    remember(nk, ix_name[0] ? ix_name : ix_id);
}

static void index_line(char* l)
{
    if (l[0] == ' ' && l[1] == ' ' && l[2] == '"') {
        copy_value(l + 3, ix_cat, sizeof(ix_cat));
        ix_id[0] = ix_ver[0] = ix_name[0] = 0;
        return;
    }
    if (!sceClibStrncmp(l, "    \"", 5)) {
        char* k = l + 5;
        if (!sceClibStrncmp(k, "titleid\": \"", 11))
            copy_value(k + 11, ix_id, sizeof(ix_id));
        else if (!sceClibStrncmp(k, "id\": \"", 6))
            copy_value(k + 6, ix_id, sizeof(ix_id));
        else if (!sceClibStrncmp(k, "version\": \"", 11))
            copy_value(k + 11, ix_ver, sizeof(ix_ver));
        else if (!sceClibStrncmp(k, "name\": \"", 8))
            copy_value(k + 8, ix_name, sizeof(ix_name));
        return;
    }
    if (!sceClibStrncmp(l, "   }", 4)) {
        item_done();
        ix_id[0] = ix_ver[0] = ix_name[0] = 0;
    }
}

static int carry;
static void (*line_fn)(char* l);

static int line_sink(const char* d, int n)
{
    for (int i = 0; i < n; i++) {
        char c = d[i];
        if (c == '\n') {
            line[carry] = 0;
            if (carry && line[carry - 1] == '\r')
                line[carry - 1] = 0;
            line_fn(line);
            carry = 0;
        } else if (carry < 2047) {
            line[carry++] = c;
        }
    }
    return 1;
}

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

static void dlog(const char* fmt, ...);

static void notify(const char* text)
{
    if (!lsdb_send)
        taiGetModuleExportFunc("SceLsdb", 0xFFFFFFFF, 0x315B9FD6, (uintptr_t*)&lsdb_send);
    if (!notice_init || !set_utf8 || !notice_clean || !lsdb_send) {
        dlog("notify skipped: no shell funcs (%d%d%d%d)", notice_init != 0, set_utf8 != 0, notice_clean != 0, lsdb_send != 0);
        return;
    }
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
    int r = lsdb_send(data, 1);
    notice_clean(data);
    dlog("notify 0x%08X: %s", (unsigned)r, text);
}

static void check(void)
{
    char cfg[512], repo[128], branch[64], url[320];
    read_file(DIR "/config.txt", cfg, sizeof(cfg));
    if (!config_value(cfg, "repo", repo, sizeof(repo)))
        sceClibSnprintf(repo, sizeof(repo), "Risaro/vpkgj-db");
    if (!config_value(cfg, "branch", branch, sizeof(branch)))
        sceClibSnprintf(branch, sizeof(branch), "main");
    read_file(DIR "/installed.txt", installed, 16 * 1024);
    read_file(DIR "/game_versions.txt", versions, 16 * 1024);
    read_file(DIR "/notified.txt", notified, 16 * 1024);
    fresh_len = 0;
    fresh[0] = 0;
    found = 0;
    first_name[0] = 0;
    if (installed[0]) {
        sceClibSnprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/index.json", repo, branch);
        carry = 0;
        ix_cat[0] = ix_id[0] = ix_ver[0] = ix_name[0] = 0;
        line_fn = index_line;
        http_get(url, line_sink);
    }
    if (versions[0]) {
        char pkgj[2048];
        if (!read_file("ux0:pkgj/config.txt", pkgj, sizeof(pkgj)))
            read_file("ur0:pkgj/config.txt", pkgj, sizeof(pkgj));
        if (config_value(pkgj, "url_updates", url, sizeof(url))) {
            carry = 0;
            line_fn = patch_line;
            http_get(url, line_sink);
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

static void dlog(const char* fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int n = sceClibSnprintf(text, sizeof(text), "%u ", (unsigned)(sceKernelGetSystemTimeWide() / 1000000ull));
    n += sceClibVsnprintf(text + n, sizeof(text) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(text) - 2)
        n = sizeof(text) - 2;
    text[n++] = '\n';
    if (file_size(DIR "/daemon.log") > 32 * 1024) {
        sceIoRemove(DIR "/daemon.old");
        sceIoRename(DIR "/daemon.log", DIR "/daemon.old");
    }
    append_file(DIR "/daemon.log", text, n);
}

static unsigned long long beat_at;

static void beat(void)
{
    unsigned long long t = sceKernelGetSystemTimeWide() / 1000000ull;
    if (beat_at && t - beat_at < 30)
        return;
    beat_at = t;
    char text[32];
    int n = sceClibSnprintf(text, sizeof(text), "%u\n", (unsigned)t);
    write_file(DIR "/daemon.alive", text, n);
}

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

static SceUID dl_fd;
static unsigned long long dl_have, dl_total, dl_done;
static int dl_fill, dl_half, dl_bad;
static const char* dl_part;

static int dl_flush(void)
{
    if (dl_fill && sceIoWrite(dl_fd, chunk, dl_fill) != dl_fill)
        dl_bad = 1;
    dl_fill = 0;
    return !dl_bad;
}

static int dl_head(int status, unsigned long long len)
{
    if (status == 416)
        sceIoRemove(dl_part);
    if (status != 200 && status != 206)
        return 0;
    if (status == 200)
        dl_have = 0;
    dl_total = len ? dl_have + len : 0;
    dl_done = dl_have;
    dl_half = dl_total ? (int)(dl_have * 4 / dl_total) : 4;
    save_state(dl_have, dl_total);
    dl_fd = sceIoOpen(dl_part, SCE_O_WRONLY | SCE_O_CREAT | (dl_have ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
    return dl_fd >= 0;
}

static int dl_sink(const char* d, int n)
{
    while (n > 0) {
        int k = CHUNK - dl_fill;
        if (k > n)
            k = n;
        sceClibMemcpy(chunk + dl_fill, d, k);
        dl_fill += k;
        d += k;
        n -= k;
        dl_done += k;
        if (dl_fill == CHUNK && !dl_flush())
            return 0;
    }
    beat();
    if (dl_total >= 32ull * 1024 * 1024) {
        int q = (int)(dl_done * 4 / dl_total);
        if (q > dl_half && q < 4) {
            dl_half = q;
            char text[200];
            unsigned mb = (unsigned)(dl_done >> 20), all = (unsigned)(dl_total >> 20);
            sceClibSnprintf(text, sizeof(text), "%s: %d%% (%u из %u МБ)", cur_name, q * 25, mb, all);
            notify(text);
        }
    }
    if (dl_done - state_at >= 512 * 1024) {
        state_at = dl_done;
        save_state(dl_done, dl_total);
        drain_queue();
        if (stop_requested())
            return 0;
    }
    return 1;
}

static int download(const char* url, const char* path)
{
    char part[200];
    sceClibSnprintf(part, sizeof(part), "%s.part", path);
    long long have = file_size(part);
    dl_have = have < 0 ? 0 : (unsigned long long)have;
    dl_part = part;
    dl_fd = -1;
    dl_fill = 0;
    dl_bad = 0;
    int ok = dnet_get(url, dl_have, dl_head, dl_sink);
    if (dl_fd >= 0) {
        if (!dl_flush())
            ok = 0;
        sceIoClose(dl_fd);
        dl_fd = -1;
    }
    if (ok && dl_total && dl_done < dl_total)
        ok = 0;
    if (!ok)
        dlog("http %d err 0x%08X", dnet_status, (unsigned)dnet_err);
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
        sceIoRemove(DIR "/bgq.stop");
        sceIoRemove(DIR "/bgstate.txt");
        fails = 0;
        retry_at = 0;
        dlog("stop");
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
    dlog("download %s", cur_name);
    static char started_key[128];
    if (sceClibStrncmp(started_key, cur_key, sizeof(started_key))) {
        sceClibSnprintf(started_key, sizeof(started_key), "%s", cur_key);
        char text[160];
        sceClibSnprintf(text, sizeof(text), "Качаю в фоне: %s", cur_name);
        notify(text);
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
            dlog("download failed %d: %s", fails, cur_name);
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
    dlog("done %s", cur_name);
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

static int move_tree(const char* src, const char* dst)
{
    sceIoMkdir(dst, 0777);
    SceUID d = sceIoDopen(src);
    if (d < 0)
        return 0;
    int ok = 1;
    SceIoDirent e;
    for (;;) {
        sceClibMemset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0)
            break;
        if (!e.d_name[0] || !sceClibStrcmp(e.d_name, ".") || !sceClibStrcmp(e.d_name, ".."))
            continue;
        char s2[320], d2[320];
        sceClibSnprintf(s2, sizeof(s2), "%s/%s", src, e.d_name);
        sceClibSnprintf(d2, sizeof(d2), "%s/%s", dst, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) {
            if (!move_tree(s2, d2))
                ok = 0;
        } else {
            sceIoRemove(d2);
            int r = sceIoRename(s2, d2);
            if (r < 0) {
                dlog("update move %s: 0x%08X", d2, (unsigned)r);
                ok = 0;
            }
        }
    }
    sceIoDclose(d);
    sceIoRmdir(src);
    return ok;
}

static unsigned long long update_seen;

static int apply_update(void)
{
    if (!exists(DIR "/update.ready")) {
        update_seen = 0;
        return 0;
    }
    if (!update_seen) {
        update_seen = now_s();
        dlog("update ready, waiting for app to close");
        return 1;
    }
    if (now_s() - update_seen < 3)
        return 1;
    char ver[32];
    read_file(DIR "/update.ready", ver, sizeof(ver));
    if (!exists(DIR "/update/eboot.bin")) {
        dlog("update without eboot, dropped");
        sceIoRemove(DIR "/update.ready");
        return 0;
    }
    dlog("update apply %s", ver);
    int ok = move_tree(DIR "/update", "ux0:app/" APP_ID);
    sceIoRemove(DIR "/update.ready");
    update_seen = 0;
    char text[160];
    if (ok) {
        write_file(DIR "/update.done", ver, slen(ver));
        sceClibSnprintf(text, sizeof(text), "VPKGJ обновлён до %s", ver);
    } else {
        sceClibSnprintf(text, sizeof(text), "Не удалось обновить VPKGJ до %s, открой его и повтори", ver);
    }
    notify(text);
    int r = sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" APP_ID);
    dlog("update %s, launch 0x%08X", ok ? "done" : "failed", (unsigned)r);
    return 0;
}

static int daemon_thread(SceSize args, void* argp)
{
    sceIoMkdir(DIR, 0777);
    beat();
    dlog("start, shell 0x%08X, notices %d", shell_nid, notice_init != 0);
    dnet_log = dlog;
    for (int i = 0; i < 3; i++) {
        sceKernelDelayThread(5 * 1000 * 1000);
        beat();
    }
    int lr = taiGetModuleExportFunc("SceLsdb", 0xFFFFFFFF, 0x315B9FD6, (uintptr_t*)&lsdb_send);
    dlog("lsdb 0x%08X %d", (unsigned)lr, lsdb_send != 0);
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (sceNetShowNetstat() == SCE_NET_ERROR_ENOTINIT) {
        SceNetInitParam p;
        SceUID blk = sceKernelAllocMemBlock("vpkgj_net", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 144 * 1024, 0);
        p.memory = 0;
        if (blk >= 0)
            sceKernelGetMemBlockBase(blk, &p.memory);
        dlog("net init needed, mem %d", p.memory != 0);
        p.size = NET_SIZE;
        p.flags = 0;
        if (p.memory)
            sceNetInit(&p);
    }
    sceNetCtlInit();
    dlog("net ready, online %d", online());
    unsigned long long next_check = now_s() + 60;
    for (;;) {
        beat();
        SceIoStat st;
        int off = sceIoGetstat(DIR "/daemon_off", &st) >= 0;
        if (!off && apply_update()) {
            sceKernelDelayThread(1000 * 1000);
            continue;
        }
        if (!off) {
            drain_queue();
            if (process_queue()) {
                sceKernelDelayThread(300 * 1000);
                continue;
            }
            if (now_s() >= next_check && online() && !exists(DIR "/bgq.txt") && !exists(DIR "/bgq.add")) {
                dlog("check");
                check();
                dlog("check done, found %d", found);
                next_check = now_s() + 6 * 3600;
            }
        } else {
            sceIoRemove(DIR "/notify.txt");
        }
        sceKernelDelayThread(2 * 1000 * 1000);
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
        break;
    }
    shell_nid = info.module_nid;
    taipool_init(16 * 1024 + CHUNK + 2048 + 16 * 1024 * 4 + 4096 + QUEUE_MAX * 2 + TLS_POOL);
    chunk = taipool_alloc(CHUNK);
    line = taipool_alloc(2048);
    installed = taipool_alloc(16 * 1024);
    versions = taipool_alloc(16 * 1024);
    notified = taipool_alloc(16 * 1024);
    fresh = taipool_alloc(16 * 1024);
    queue_buf = taipool_alloc(4096);
    bgq = taipool_alloc(QUEUE_MAX);
    bgadd = taipool_alloc(QUEUE_MAX);
    dnet_heap(taipool_alloc(TLS_POOL), TLS_POOL);
    if (!bgq || !bgadd || !queue_buf || !chunk || !line || !installed || !versions || !notified || !fresh)
        return SCE_KERNEL_START_FAILED;
    SceUID th = sceKernelCreateThread("vpkgj_daemon", daemon_thread, 0xB0, 0x20000, 0, 0, 0);
    if (th >= 0)
        sceKernelStartThread(th, 0, 0);
    return SCE_KERNEL_START_SUCCESS;
}

int _start(SceSize argc, const void* args) __attribute__((weak, alias("module_start")));

int module_stop(SceSize argc, const void* args)
{
    return SCE_KERNEL_STOP_SUCCESS;
}
