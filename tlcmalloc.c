/**
 * Threadless Cache Malloc
 * Inspired by tcmalloc but for single-threaded applications.
 *
 * We split allocations into small and large objects around 32 kB. Small
 * objects are split into size classes of powers-of-two, each with its own
 * free list. New arenas are allocated in pages.
 *
 * Large objects are rounded up to the nearest page size and allocated as runs
 * of pages. Each freed large object is added to a list of large objects with
 * the same number of pages up to 255, after which they're all part of the same
 * list.
**/

struct FreeSmob {
    struct FreeSmob *next;
};

static struct FreeSmob *smob[15];
static void *lobj[256];

void *malloc()