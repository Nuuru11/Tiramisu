// tiramisu/tests/test_x86_image.cpp
// Gate for PE32 image execution: build a minimal REAL PE32, map it through
// trt_map_pe32, and run its entry through the clean-room interpreter.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"
#include "../src/trt/x86_32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

// Minimal host stubs so the PE32 mapping path can link without the full
// bridge set (the interpreter gate is about x86-32 execution, not Win32).
extern "C" {
int trt_mem_budget_charge_commit(u64) { return 0; }
void trt_mem_budget_discharge_commit(u64) {}
int trt_mem_budget_charge_va(u64) { return 0; }
void trt_mem_budget_discharge_va(u64) {}
u32 TRT_STDCALL trt_GetCurrentThreadId(void) { return 1; }
void trt_eh_retire_resume_records(unsigned) {}
void trt_bridge_post_quit(int) {}
}

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { ++fails; printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static void* be_alloc(size_t n, u64) {
  return mmap(nullptr, n, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static int be_protect(void* p, size_t n, unsigned prot) {
  int m = PROT_READ;
  if (prot & TRT_PROT_W) m |= PROT_WRITE;
  if (prot & TRT_PROT_X) m |= PROT_EXEC;
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  return mprotect((void*)((uintptr_t)p & ~(page - 1)), n + page, m);
}
static void be_release(void* p, size_t n) { munmap(p, n); }
static void be_log(const char* line) { (void)line; }
static trt_backend be{be_alloc, be_protect, be_release, be_log, nullptr};
static void* rs_resolve(void*, const char*, const char*, u16) { return nullptr; }
static trt_resolver rs{nullptr, rs_resolve};

static void put16(u8* p, u16 v) { memcpy(p, &v, 2); }
static void put32(u8* p, u32 v) { memcpy(p, &v, 4); }

// Minimal PE32: one .text section at RVA 0x1000, entry = mov eax,7 ; hlt.
static size_t make_pe32(u8* out) {
  memset(out, 0, 0x400);
  out[0] = 'M'; out[1] = 'Z';
  put32(out + 0x3c, 0x40);
  u8* pe = out + 0x40;
  pe[0] = 'P'; pe[1] = 'E'; pe[2] = 0; pe[3] = 0;
  put16(pe + 4, 0x014c);            // i386
  put16(pe + 6, 1);                 // one section
  put16(pe + 20, 224);              // size of optional header
  u8* opt = pe + 24;
  put16(opt, 0x10b);                // PE32 magic
  put32(opt + 16, 0x1000);          // AddressOfEntryPoint
  put32(opt + 28, 0x10000000);      // ImageBase
  put32(opt + 32, 0x1000);          // SectionAlignment
  put32(opt + 36, 0x200);           // FileAlignment
  put32(opt + 56, 0x2000);          // SizeOfImage
  put32(opt + 60, 0x200);           // SizeOfHeaders
  put16(opt + 68, 3); put16(opt + 70, 0);  // subsystem = console
  put32(opt + 92, 16);              // NumberOfRvaAndSizes
  // import dir (index 1) left empty: no imports, so no traps
  u8* sec = opt + 224;
  memcpy(sec, ".text\0\0", 8);
  put32(sec + 8, 0x200);            // VirtualSize
  put32(sec + 12, 0x1000);          // VirtualAddress
  put32(sec + 16, 0x200);           // SizeOfRawData
  put32(sec + 20, 0x200);           // PointerToRawData
  put32(sec + 36, 0x60000020);      // code | execute | read
  u8* code = out + 0x200;
  code[0] = 0xb8; put32(code + 1, 7);
  code[5] = 0xf4;                   // hlt
  return 0x400;
}

int main() {
  trt_runtime_init(&be, "");
  u8 file[0x400];
  size_t n = make_pe32(file);

  trt_image* img = nullptr;
  int rc = trt_map_pe32(&be, &rs, file, n, &img);
  CHECK(rc == TRT_OK && img != nullptr, "minimal PE32 mapped");
  if (!img) { printf("  result: FAIL\n"); return 1; }
  CHECK(img->is_pe32 == 1, "image flagged PE32");
  CHECK(img->entry_rva == 0x1000, "entry RVA parsed from PE32 header");

  u32 eax = 0;
  char trap[160] = "";
  rc = trt_x86_run_image(img, img->guest_base + (u32)img->entry_rva,
                         0x10000, 64, &eax, trap, sizeof(trap));
  CHECK(rc == 0, "PE32 entry ran to HLT through the interpreter");
  CHECK(eax == 7, "PE32 entry produced EAX=7 from real x86-32 code");

  trt_images_reset();
  printf("  result: %s\n", fails ? "FAIL" : "ALL PASS");
  return fails ? 1 : 0;
}
