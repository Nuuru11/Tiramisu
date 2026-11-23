// tiramisu/src/trt/universal_guest.cpp — Phase 4 universal guest runner.
#include "universal_guest.h"

#include <fstream>
#include <iterator>
#include <vector>

trt_image* trt_universal_guest_load(const char* guest_exe,
                                    const trt_backend* be,
                                    const trt_resolver* rs) {
  if (!guest_exe || !guest_exe[0] || !be || !rs) return nullptr;
  std::ifstream f(guest_exe, std::ios::binary);
  if (!f) return nullptr;
  std::vector<u8> buf((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  if (buf.empty()) return nullptr;
  trt_image* img = nullptr;
  if (trt_map_pe(be, rs, buf.data(), buf.size(), &img) != TRT_OK)
    return nullptr;
  return img;
}

u64 trt_universal_guest_entry(trt_image* img) {
  return img ? (u64)img->entry_rva : 0;
}

void trt_universal_guest_unload(const trt_backend* be, trt_image* img) {
  if (img) trt_unmap(be, img);
}
