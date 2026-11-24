// tiramisu/src/trt/bridge_heap.cpp
// Heap registry + slab pool, extracted from bridge_kernel32.cpp.
//
// Why the pool exists: HeapAlloc used to cost one backend VirtualAlloc/mmap
// per guest allocation (one syscall + one page commit per block, even for 16
// bytes). The pool carves small blocks out of 1 MB slabs reserved from the
// backend, so a game doing thousands of small allocations pays one syscall
// per slab instead of one per block.
//
// Budget accounting charges the SLAB reservation (1 MB commit + 1 MB VA at
// slab creation, discharged when the slab is released) — never per-block
// carving, so the ledger always equals real committed memory. Blocks over
// TRT_HEAP_POOL_MAX bypass the pool and are charged per-allocation by the
// caller, exactly as before; the small-budget probe's 10 MB ledger
// assertions depend on that split.
//
// Layout: a slab is 1 MB, page-aligned from the backend. Blocks start at
// offset 256; each block is a 16-byte header (slab pointer + magic) plus a
// class-sized payload, so every returned pointer is 16-aligned. The free
// list links through the user area's first 8 bytes (dead while the block is
// free). All pool and registry state shares one mutex, so there is no lock
// ordering to get wrong.
#include "bridge_heap.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "thunk.h"

// Memory budget ledger (bridge_membudget.cpp).
extern "C" int trt_mem_budget_charge_commit(u64 bytes);
extern "C" int trt_mem_budget_charge_va(u64 bytes);
extern "C" void trt_mem_budget_discharge_commit(u64 bytes);
extern "C" void trt_mem_budget_discharge_va(u64 bytes);

namespace {

// ---- registry: HeapSize/HeapReAlloc need the byte size of every block
// HeapAlloc handed out, plus which backend owns it (pool slab vs direct
// backend alloc). The high bit of the recorded size marks a DIRECT block
// (backend alloc, charged per-alloc); a clear high bit marks a POOL block
// (carved from a charged slab). The _locked helpers assume g_heap_mu is
// held; the exported wrappers lock.
constexpr size_t k_heap_direct_flag = (size_t)1 << (sizeof(size_t) * 8 - 1);
std::mutex g_heap_mu;
std::unordered_map<void*, size_t> g_heap_rec;

bool heap_rec_find_locked(void* p, size_t* size_out, bool* direct_out) {
  if (!p) return false;
  auto it = g_heap_rec.find(p);
  if (it == g_heap_rec.end()) return false;
  if (size_out) *size_out = it->second & ~k_heap_direct_flag;
  if (direct_out) *direct_out = (it->second & k_heap_direct_flag) != 0;
  return true;
}
void heap_rec_add_locked(void* p, size_t n, bool direct) {
  if (!p) return;
  g_heap_rec[p] = (n & ~k_heap_direct_flag) | (direct ? k_heap_direct_flag : 0);
}
void heap_rec_del_locked(void* p) {
  if (!p) return;
  g_heap_rec.erase(p);
}

// ---- slab pool ----
constexpr size_t k_heap_slab_bytes = 1024 * 1024;  // 1 MB per slab
constexpr size_t k_heap_slab_hdr = 256;            // slab header reservation
constexpr size_t k_heap_pool_max = TRT_HEAP_POOL_MAX;
constexpr u64 k_heap_block_magic = 0x484541504F4F4C21ULL;  // "HEAPOOL!"
constexpr u64 k_heap_slab_magic = 0x48454150534C4142ULL;   // "HEAPSLAB"
constexpr size_t k_heap_classes[] = {16,    32,    64,    128,   256,   512, 1024,
                                     2048, 4096,  8192,  16384, 32768, 65536};
constexpr size_t k_heap_nclasses =
    sizeof(k_heap_classes) / sizeof(k_heap_classes[0]);

struct heap_slab {
  u64 magic;         // k_heap_slab_magic
  u32 class_idx;
  u32 block_stride;  // 16 (header) + class size
  u32 live;          // blocks currently handed out
  u32 capacity;      // blocks the slab holds
  u32 next_off;      // bump offset for carving (bytes from slab base)
  void* free_list;   // singly-linked free blocks (link in user area)
  heap_slab* next;   // next slab in this class
};

struct heap_block_hdr {
  heap_slab* slab;
  u64 magic;  // k_heap_block_magic
};

heap_slab* g_heap_slabs[k_heap_nclasses] = {};

int heap_class_for(size_t n) {
  for (size_t i = 0; i < k_heap_nclasses; ++i)
    if (n <= k_heap_classes[i]) return (int)i;
  return -1;
}

// Carve or reuse a block from the class's slabs; creates a slab (charged)
// when every slab is full. Returns nullptr (with 1455/8 set) when the budget
// refuses the slab reservation. g_heap_mu must be held.
void* heap_pool_alloc_locked(size_t n) {
  const int ci = heap_class_for(n);
  if (ci < 0) return nullptr;
  const size_t stride = 16 + k_heap_classes[ci];
  for (heap_slab* s = g_heap_slabs[ci]; s; s = s->next) {
    if (s->free_list) {
      u8* blk = (u8*)s->free_list;
      s->free_list = *(void**)(blk + 16);  // link lives in the user area
      ++s->live;
      return blk + 16;
    }
    if (s->next_off + stride <= k_heap_slab_bytes) {
      u8* blk = (u8*)s + s->next_off;
      s->next_off += (u32)stride;
      heap_block_hdr* h = (heap_block_hdr*)blk;
      h->slab = s;
      h->magic = k_heap_block_magic;
      ++s->live;
      return blk + 16;
    }
  }
  // no room anywhere in this class: reserve a fresh slab (charged 1 MB
  // commit + 1 MB VA — the ledger charges the reservation, never the carving)
  if (trt_mem_budget_charge_va((u64)k_heap_slab_bytes) != 0) return nullptr;
  if (trt_mem_budget_charge_commit((u64)k_heap_slab_bytes) != 0) {
    trt_mem_budget_discharge_va((u64)k_heap_slab_bytes);
    return nullptr;
  }
  const trt_backend* be = trt_core_backend();
  u8* mem = be ? (u8*)be->alloc(k_heap_slab_bytes, 0) : nullptr;
  if (!mem) {
    trt_mem_budget_discharge_commit((u64)k_heap_slab_bytes);
    trt_mem_budget_discharge_va((u64)k_heap_slab_bytes);
    return nullptr;
  }
  heap_slab* ns = (heap_slab*)mem;
  ns->magic = k_heap_slab_magic;
  ns->class_idx = (u32)ci;
  ns->block_stride = (u32)stride;
  ns->live = 1;
  ns->capacity = (u32)((k_heap_slab_bytes - k_heap_slab_hdr) / stride);
  ns->next_off = (u32)(k_heap_slab_hdr + stride);
  ns->free_list = nullptr;
  ns->next = g_heap_slabs[ci];
  g_heap_slabs[ci] = ns;
  u8* blk = mem + k_heap_slab_hdr;
  heap_block_hdr* h = (heap_block_hdr*)blk;
  h->slab = ns;
  h->magic = k_heap_block_magic;
  return blk + 16;
}

// Return a pool block to its slab; releases the slab (and discharges its
// reservation) when the last block goes. Returns 1 on a clean free, 0 when
// the block header is corrupt (fail-visible: the block leaks rather than
// trusting a forged slab pointer). The owning slab is found by range
// containment over the class's live slabs — the header's slab pointer is
// never dereferenced untrusted. g_heap_mu must be held.
int heap_pool_free_locked(void* p, size_t n) {
  u8* blk = (u8*)p - 16;
  heap_block_hdr* h = (heap_block_hdr*)blk;
  if (h->magic != k_heap_block_magic) {
    char line[192];
    snprintf(line, sizeof(line),
             "kernel32: HeapFree corruption: block %p header magic mismatch "
             "(not a pool block or heap overwritten) — leaked, not freed",
             p);
    trt_core_log(line);
    return 0;
  }
  const int ci = heap_class_for(n);
  heap_slab* s = nullptr;
  if (ci >= 0) {
    for (heap_slab* c = g_heap_slabs[ci]; c; c = c->next) {
      const uintptr_t lo = (uintptr_t)c;
      const uintptr_t hi = lo + k_heap_slab_bytes;
      if ((uintptr_t)blk >= lo && (uintptr_t)blk < hi) {
        s = c;
        break;
      }
    }
  }
  if (!s || s->magic != k_heap_slab_magic) {
    trt_core_log("kernel32: HeapFree corruption: pool block names no live "
                 "slab — leaked, not freed");
    return 0;
  }
  if (h->slab != s) {
    trt_core_log("kernel32: HeapFree corruption: block header slab disagrees "
                 "with the live slab list (proceeding with the live slab)");
  }
  *(void**)p = s->free_list;  // link into the free list via the user area
  s->free_list = blk;
  if (--s->live == 0) {
    // last live block: release the slab and discharge its reservation
    const u32 sci = s->class_idx;
    heap_slab** pp = &g_heap_slabs[sci];
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    const trt_backend* be = trt_core_backend();
    if (be) be->release(s, k_heap_slab_bytes);
    trt_mem_budget_discharge_commit((u64)k_heap_slab_bytes);
    trt_mem_budget_discharge_va((u64)k_heap_slab_bytes);
  }
  return 1;
}

}  // namespace

extern "C" void* trt_heap_pool_alloc(size_t bytes) {
  if (bytes == 0) bytes = 1;
  if (bytes > k_heap_pool_max) return nullptr;
  std::lock_guard<std::mutex> lk(g_heap_mu);
  void* p = heap_pool_alloc_locked(bytes);
  if (p) heap_rec_add_locked(p, bytes, false);
  return p;
}

extern "C" int trt_heap_rec_peek(void* p, size_t* size_out, int* direct_out) {
  bool direct = false;
  bool found;
  {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    found = heap_rec_find_locked(p, size_out, &direct);
  }
  if (direct_out) *direct_out = direct ? 1 : 0;
  return found ? 1 : 0;
}

extern "C" void trt_heap_rec_add(void* p, size_t bytes, int direct) {
  if (!p) return;
  std::lock_guard<std::mutex> lk(g_heap_mu);
  heap_rec_add_locked(p, bytes, direct != 0);
}

extern "C" void trt_heap_rec_del(void* p) {
  if (!p) return;
  std::lock_guard<std::mutex> lk(g_heap_mu);
  heap_rec_del_locked(p);
}

extern "C" int trt_heap_release(void* p, size_t* size_out, int* direct_out) {
  if (!p) return -1;
  size_t n = 0;
  bool direct = false;
  {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    if (!heap_rec_find_locked(p, &n, &direct)) return -1;
    heap_rec_del_locked(p);  // drop the size record (HeapSize honesty)
    if (!direct) {
      if (!heap_pool_free_locked(p, n)) return 0;  // corrupt: logged, leaked
    }
  }
  if (direct) {
    // backend tracks the size internally (mmap/UWP), as before
    const trt_backend* be = trt_core_backend();
    if (be) be->release(p, 0);
  }
  if (size_out) *size_out = n;
  if (direct_out) *direct_out = direct ? 1 : 0;
  return 1;
}
