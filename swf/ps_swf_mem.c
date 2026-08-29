#include "ps_swf.h"
#include "ps_swf_mem.h"

#include <stdlib.h>

/* Two size_t of prefix rather than one: the payload has to stay maximally
 * aligned, and on both the host and SH-4 that means the header must be a
 * multiple of the largest alignment the structures need. */
#define MEM_HDR (2 * sizeof(size_t))

static size_t mem_live, mem_peak;

static void note(void)
{
    if(mem_live > mem_peak)
        mem_peak = mem_live;
}

void *ps_swf_alloc(size_t n)
{
    unsigned char *p = malloc(n + MEM_HDR);

    if(!p)
        return NULL;
    *(size_t *)p = n;
    mem_live += n;
    note();
    return p + MEM_HDR;
}

void *ps_swf_realloc(void *v, size_t n)
{
    unsigned char *p   = v ? (unsigned char *)v - MEM_HDR : NULL;
    size_t         old = p ? *(size_t *)p : 0;
    unsigned char *q   = realloc(p, n + MEM_HDR);

    if(!q)
        return NULL;
    *(size_t *)q = n;
    mem_live += n - old;
    note();
    return q + MEM_HDR;
}

void ps_swf_dealloc(void *v)
{
    unsigned char *p;

    if(!v)
        return;
    p = (unsigned char *)v - MEM_HDR;
    mem_live -= *(size_t *)p;
    free(p);
}

size_t ps_swf_mem_live(void) { return mem_live; }
size_t ps_swf_mem_peak(void) { return mem_peak; }

void ps_swf_mem_reset_peak(void) { mem_peak = mem_live; }
