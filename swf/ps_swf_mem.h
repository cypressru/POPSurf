/* Size-tracking allocator for everything the SWF player holds.
 *
 * Not an arena and not a pool - just malloc with a size prefix and a running
 * total, so "what did this file cost, and what did it peak at while parsing"
 * has an answer. That question is unavoidable on the target: 16MB total, no
 * virtual memory, no swap, and a page that can hand us a .swf of its own
 * choosing. Answering it on the host, per file, is much cheaper than
 * discovering the answer as a lock-up on hardware.
 *
 * Deliberately not exposed in ps_swf.h: callers see only the two totals.
 */
#ifndef PS_SWF_MEM_H
#define PS_SWF_MEM_H

#include <stddef.h>

void *ps_swf_alloc(size_t n);
void *ps_swf_realloc(void *p, size_t n);
void  ps_swf_dealloc(void *p);

#endif /* PS_SWF_MEM_H */
