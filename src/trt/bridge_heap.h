// tiramisu/src/trt/bridge_heap.h
// Internal heap subsystem for the kernel32 bridge.
//
// This lived inside bridge_kernel32.cpp: ~200 lines of registry and slab-pool
// state sitting in a 9,000-line translation unit, so every unrelated kernel32
// edit recompiled all of it and nothing could test the allocator without
// dragging in 49 bridge headers. The guest-visible HeapAlloc/HeapFree/
// HeapSize/HeapReAlloc remain in bridge_kernel32.cpp (they own the budget
// policy); this file owns the memory and the bookkeeping.
//
// Not part of the guest ABI: these are internal C-linkage helpers.
#ifndef TRT_BRIDGE_HEAP_H
#define TRT_BRIDGE_HEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Largest request served from the slab pool; bigger ones go to the backend.
#define TRT_HEAP_POOL_MAX ((size_t)65536)

// Carve a block from the slab pool (allocating and charging a 1 MB slab when
// the class is full) and record it. Returns NULL when the budget refuses the
// slab reservation; the charge helpers have already set the guest error.
void* trt_heap_pool_alloc(size_t bytes);

// Recorded size of a block handed out by HeapAlloc. Returns 1 when the pointer
// is known, 0 otherwise. direct_out (optional) reports a backend-allocated
// block rather than a pooled one.
int trt_heap_rec_peek(void* p, size_t* size_out, int* direct_out);

// Record / drop a block. add() is a no-op for NULL.
void trt_heap_rec_add(void* p, size_t bytes, int direct);
void trt_heap_rec_del(void* p);

// Release a recorded block's memory:
//   1  released; *size_out / *direct_out describe what went (if non-NULL).
//      A pooled block returns to its slab (the slab itself is released and
//      discharged when its last block goes). A direct block is released
//      through the backend.
//   0  pooled block header is corrupt: logged and deliberately leaked rather
//      than trusting a forged slab pointer.
//  -1  the pointer was never recorded: the caller decides what to do.
// The budget ledger is NOT touched here — realloc's discharge rules differ
// from plain free's, so the caller applies them.
int trt_heap_release(void* p, size_t* size_out, int* direct_out);

#ifdef __cplusplus
}
#endif

#endif  // TRT_BRIDGE_HEAP_H
