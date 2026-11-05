#include "dnet.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl.h>

#include <stdarg.h>

int dnet_err;
int dnet_status;
void (*dnet_log)(const char* fmt, ...);

#define LOG(...) do { if (dnet_log) dnet_log(__VA_ARGS__); } while (0)

typedef struct {
    unsigned size;
    unsigned used;
} Blk;

static unsigned char* heap;
static unsigned heap_size;

void dnet_heap(void* mem, int size)
{
    unsigned pad = mem ? (8 - ((unsigned)(unsigned long)mem & 7)) & 7 : 0;
    heap = mem ? (unsigned char*)mem + pad : 0;
    heap_size = mem ? ((unsigned)size - pad) & ~7u : 0;
    if (heap) {
        Blk* b = (Blk*)heap;
        b->size = heap_size;
        b->used = 0;
    }
}

static void heap_merge(void)
{
    unsigned off = 0;
    while (off < heap_size) {
        Blk* b = (Blk*)(heap + off);
        while (!b->used && off + b->size < heap_size) {
            Blk* n = (Blk*)(heap + off + b->size);
            if (n->used)
                break;
            b->size += n->size;
        }
        off += b->size;
    }
}

void* tls_calloc(size_t n, size_t size)
{
    if (!heap || !n || !size || size > 0x1000000 / n)
        return 0;
    unsigned need = ((unsigned)(n * size) + sizeof(Blk) + 7) & ~7u;
    for (int pass = 0; pass < 2; pass++) {
        unsigned off = 0;
        while (off < heap_size) {
            Blk* b = (Blk*)(heap + off);
            if (!b->used && b->size >= need) {
                if (b->size - need >= sizeof(Blk) + 16) {
                    Blk* r = (Blk*)(heap + off + need);
                    r->size = b->size - need;
                    r->used = 0;
                    b->size = need;
                }
                b->used = 1;
                sceClibMemset(b + 1, 0, b->size - sizeof(Blk));
                return b + 1;
            }
            off += b->size;
        }
        heap_merge();
    }
    return 0;
}

void tls_free(void* p)
{
    if (!p)
        return;
    Blk* b = (Blk*)p - 1;
    b->used = 0;
    unsigned off = (unsigned)((unsigned char*)b - heap) + b->size;
    while (off < heap_size) {
        Blk* n = (Blk*)(heap + off);
        if (n->used)
            break;
        b->size += n->size;
        off += n->size;
    }
}

int tls_snprintf(char* s, size_t n, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = sceClibVsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

void* memcpy(void* d, const void* s, size_t n)
{
    return sceClibMemcpy(d, s, n);
}

void* memmove(void* d, const void* s, size_t n)
{
    return sceClibMemmove(d, s, n);
}

void* memset(void* d, int c, size_t n)
{
    return sceClibMemset(d, c, n);
}

int memcmp(const void* a, const void* b, size_t n)
{
    return sceClibMemcmp(a, b, n);
}

size_t strlen(const char* s)
{
    return sceClibStrnlen(s, 0x7FFFFFFF);
}

int strcmp(const char* a, const char* b)
{
    return sceClibStrcmp(a, b);
}

int strncmp(const char* a, const char* b, size_t n)
{
    return sceClibStrncmp(a, b, n);
}

char* strchr(const char* s, int c)
{
    return sceClibStrchr(s, c);
}

char* strstr(const char* a, const char* b)
{
    return sceClibStrstr(a, b);
}

int inet_pton(int af, const char* src, void* dst)
{
    return sceNetInetPton(af, src, dst);
}

static mbedtls_ctr_drbg_context drbg;
static mbedtls_ssl_config conf;
static int tls_ready;

static int entropy(void* p, unsigned char* out, size_t len)
{
    LOG("tls entropy %d", (int)len);
    while (len) {
        size_t n = len > 64 ? 64 : len;
        int r = sceKernelGetRandomNumber(out, n);
        if (r < 0) {
            LOG("tls rng err 0x%08X", (unsigned)r);
            return -1;
        }
        out += n;
        len -= n;
    }
    LOG("tls entropy ok");
    return 0;
}

static int tls_init(void)
{
    if (tls_ready)
        return 1;
    LOG("tls drbg init");
    mbedtls_ctr_drbg_init(&drbg);
    LOG("tls drbg seed");
    int r = mbedtls_ctr_drbg_seed(&drbg, entropy, 0, (const unsigned char*)"vpkgj", 5);
    if (r != 0) {
        LOG("tls drbg seed err -0x%04X", (unsigned)-r);
        return 0;
    }
    LOG("tls config");
    mbedtls_ssl_config_init(&conf);
    r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        LOG("tls config err -0x%04X", (unsigned)-r);
        return 0;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    LOG("tls ready");
    tls_ready = 1;
    return 1;
}

typedef struct {
    int sock;
    int tls;
    mbedtls_ssl_context ssl;
    unsigned char buf[16384];
    int pos, len;
} Conn;

static Conn conn;

static int bio_send(void* ctx, const unsigned char* b, size_t n)
{
    int r = sceNetSend(*(int*)ctx, b, n, 0);
    return r < 0 ? -0x004E : r;
}

static int bio_recv(void* ctx, unsigned char* b, size_t n)
{
    int r = sceNetRecv(*(int*)ctx, b, n, 0);
    return r < 0 ? -0x004C : r;
}

static int raw_read(Conn* c, unsigned char* b, int n)
{
    if (!c->tls) {
        int r = sceNetRecv(c->sock, b, n, 0);
        if (r < 0)
            dnet_err = r;
        return r;
    }
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (r < 0)
            dnet_err = r;
        return r;
    }
}

static int raw_write(Conn* c, const char* b, int n)
{
    while (n > 0) {
        int r;
        if (c->tls) {
            r = mbedtls_ssl_write(&c->ssl, (const unsigned char*)b, n);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
        } else {
            r = sceNetSend(c->sock, b, n, 0);
        }
        if (r <= 0) {
            dnet_err = r;
            return 0;
        }
        b += r;
        n -= r;
    }
    return 1;
}

static int fill(Conn* c)
{
    if (c->pos < c->len)
        return 1;
    int r = raw_read(c, c->buf, sizeof(c->buf));
    if (r <= 0)
        return 0;
    c->pos = 0;
    c->len = r;
    return 1;
}

static int read_line(Conn* c, char* out, int max)
{
    int i = 0;
    for (;;) {
        if (!fill(c))
            return -1;
        char ch = c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (ch != '\r' && i < max - 1)
            out[i++] = ch;
    }
    out[i] = 0;
    return i;
}

static void conn_close(Conn* c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        c->tls = 0;
    }
    if (c->sock >= 0) {
        sceNetSocketClose(c->sock);
        c->sock = -1;
    }
}

static int conn_open(Conn* c, const char* host, int port, int https)
{
    c->sock = -1;
    c->tls = 0;
    c->pos = c->len = 0;
    SceNetInAddr addr;
    LOG("net resolve %s", host);
    if (sceNetInetPton(SCE_NET_AF_INET, host, &addr) <= 0) {
        int rid = sceNetResolverCreate("vpkgj", 0, 0);
        if (rid < 0) {
            dnet_err = rid;
            return 0;
        }
        int r = sceNetResolverStartNtoa(rid, host, &addr, 0, 0, 0);
        sceNetResolverDestroy(rid);
        if (r < 0) {
            LOG("net resolve err 0x%08X", (unsigned)r);
            dnet_err = r;
            return 0;
        }
    }
    c->sock = sceNetSocket("vpkgj", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (c->sock < 0) {
        dnet_err = c->sock;
        return 0;
    }
    int tmo = 30 * 1000 * 1000;
    sceNetSetsockopt(c->sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
    sceNetSetsockopt(c->sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
    SceNetSockaddrIn sa;
    sceClibMemset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = sceNetHtons(port);
    sa.sin_addr = addr;
    LOG("net connect %d", port);
    int r = sceNetConnect(c->sock, (SceNetSockaddr*)&sa, sizeof(sa));
    if (r < 0) {
        LOG("net connect err 0x%08X", (unsigned)r);
        dnet_err = r;
        conn_close(c);
        return 0;
    }
    if (!https)
        return 1;
    LOG("net tls init");
    if (!tls_init()) {
        LOG("net tls init failed");
        dnet_err = -1;
        conn_close(c);
        return 0;
    }
    mbedtls_ssl_init(&c->ssl);
    c->tls = 1;
    LOG("tls setup");
    if ((r = mbedtls_ssl_setup(&c->ssl, &conf)) != 0 || (r = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) {
        dnet_err = r;
        conn_close(c);
        return 0;
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->sock, bio_send, bio_recv, 0);
    LOG("net handshake");
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOG("net handshake err -0x%04X", (unsigned)-r);
            dnet_err = r;
            conn_close(c);
            return 0;
        }
    }
    LOG("net handshake ok");
    return 1;
}

static int lower(int ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
}

static const char* header_value(const char* line, const char* name)
{
    int i = 0;
    for (; name[i]; i++)
        if (lower(line[i]) != name[i])
            return 0;
    if (line[i] != ':')
        return 0;
    i++;
    while (line[i] == ' ' || line[i] == '\t')
        i++;
    return line + i;
}

static unsigned long long parse_u64(const char* s, int hex)
{
    unsigned long long v = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (hex && lower(*s) >= 'a' && lower(*s) <= 'f')
            d = lower(*s) - 'a' + 10;
        else
            break;
        v = v * (hex ? 16 : 10) + d;
    }
    return v;
}

static int split_url(const char* url, int* https, char* host, int hmax, int* port, char* path, int pmax)
{
    const char* p = url;
    if (!sceClibStrncmp(p, "https://", 8)) {
        *https = 1;
        p += 8;
    } else if (!sceClibStrncmp(p, "http://", 7)) {
        *https = 0;
        p += 7;
    } else {
        return 0;
    }
    *port = *https ? 443 : 80;
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < hmax - 1)
        host[i++] = *p++;
    host[i] = 0;
    if (*p == ':') {
        p++;
        *port = (int)parse_u64(p, 0);
        while (*p >= '0' && *p <= '9')
            p++;
    }
    if (!*p)
        p = "/";
    i = 0;
    while (*p && i < pmax - 1)
        path[i++] = *p++;
    path[i] = 0;
    return host[0] != 0;
}

static char cur_url[2048];
static char line[2048];

int dnet_get(const char* url, unsigned long long from, DnetHead head, DnetSink sink)
{
    dnet_err = 0;
    dnet_status = 0;
    sceClibSnprintf(cur_url, sizeof(cur_url), "%s", url);
    for (int hop = 0; hop < 6; hop++) {
        int https, port;
        char host[256], path[1800];
        if (!split_url(cur_url, &https, host, sizeof(host), &port, path, sizeof(path))) {
            dnet_err = -2;
            return 0;
        }
        Conn* c = &conn;
        if (!conn_open(c, host, port, https))
            return 0;
        char req[2400];
        int n = sceClibSnprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VPKGJ-daemon/1.1\r\nAccept: */*\r\nConnection: close\r\n", path, host);
        if (from) {
            char num[24];
            int k = 23;
            num[k] = 0;
            unsigned long long v = from;
            do {
                num[--k] = '0' + (int)(v % 10);
                v /= 10;
            } while (v && k);
            n += sceClibSnprintf(req + n, sizeof(req) - n, "Range: bytes=%s-\r\n", num + k);
        }
        n += sceClibSnprintf(req + n, sizeof(req) - n, "\r\n");
        if (!raw_write(c, req, n)) {
            conn_close(c);
            return 0;
        }
        if (read_line(c, line, sizeof(line)) < 12 || sceClibStrncmp(line, "HTTP/1.", 7)) {
            if (!dnet_err)
                dnet_err = -3;
            conn_close(c);
            return 0;
        }
        int status = (int)parse_u64(line + 9, 0);
        dnet_status = status;
        LOG("net status %d", status);
        unsigned long long length = 0;
        int has_length = 0, chunked = 0;
        int redirect = 0;
        for (;;) {
            int l = read_line(c, line, sizeof(line));
            if (l < 0) {
                conn_close(c);
                return 0;
            }
            if (l == 0)
                break;
            const char* v;
            if ((v = header_value(line, "content-length"))) {
                length = parse_u64(v, 0);
                has_length = 1;
            } else if ((v = header_value(line, "transfer-encoding")) && lower(v[0]) == 'c') {
                chunked = 1;
            } else if ((v = header_value(line, "location")) && status >= 300 && status < 400) {
                if (v[0] == '/') {
                    char base[300];
                    sceClibSnprintf(base, sizeof(base), "%s://%s", https ? "https" : "http", host);
                    char tmp[2048];
                    sceClibSnprintf(tmp, sizeof(tmp), "%s%s", base, v);
                    sceClibSnprintf(cur_url, sizeof(cur_url), "%s", tmp);
                } else {
                    sceClibSnprintf(cur_url, sizeof(cur_url), "%s", v);
                }
                redirect = 1;
            }
        }
        if (redirect) {
            conn_close(c);
            continue;
        }
        int go = head ? head(status, has_length ? length : 0) : status == 200;
        if (!go) {
            conn_close(c);
            return 0;
        }
        int ok = 1;
        if (chunked) {
            for (;;) {
                if (read_line(c, line, sizeof(line)) < 0) {
                    ok = 0;
                    break;
                }
                unsigned long long left = parse_u64(line, 1);
                if (!left)
                    break;
                while (left) {
                    if (!fill(c)) {
                        ok = 0;
                        break;
                    }
                    int k = c->len - c->pos;
                    if ((unsigned long long)k > left)
                        k = (int)left;
                    if (!sink((const char*)c->buf + c->pos, k)) {
                        ok = 0;
                        break;
                    }
                    c->pos += k;
                    left -= k;
                }
                if (!ok || read_line(c, line, sizeof(line)) < 0) {
                    ok = 0;
                    break;
                }
            }
        } else {
            unsigned long long left = length;
            for (;;) {
                if (has_length && !left)
                    break;
                if (!fill(c)) {
                    if (has_length || dnet_err)
                        ok = 0;
                    break;
                }
                int k = c->len - c->pos;
                if (has_length && (unsigned long long)k > left)
                    k = (int)left;
                if (!sink((const char*)c->buf + c->pos, k)) {
                    ok = 0;
                    break;
                }
                c->pos += k;
                if (has_length)
                    left -= k;
            }
        }
        conn_close(c);
        return ok;
    }
    dnet_err = -4;
    return 0;
}
