#ifndef VPKGJ_DNET_H
#define VPKGJ_DNET_H

typedef int (*DnetHead)(int status, unsigned long long length);
typedef int (*DnetSink)(const char* data, int len);

extern int dnet_err;
extern int dnet_status;
extern void (*dnet_log)(const char* fmt, ...);

void dnet_heap(void* mem, int size);
int dnet_get(const char* url, unsigned long long from, DnetHead head, DnetSink sink);

#endif
