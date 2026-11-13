// tiramisu/src/guest/guest_shlwapi.c
// Proves the shlwapi bridge: the path-string work every engine does while
// assembling asset/config paths at boot, plus a VFS-backed existence check.
// Returns a bitmask:
//   0x001 PathCombineW("C:\dir", "file.txt") -> "C:\dir\file.txt"
//   0x002 PathFindFileNameW finds "engine.dll" after the last backslash
//   0x004 PathAppendW appends with the separator
//   0x008 PathStripPathW keeps only the file name
//   0x010 PathFindExtensionW points at ".cfg"
//   0x020 PathRemoveExtensionW strips it
//   0x040 PathIsRelativeW: "C:\x" no, "x\y" yes
//   0x080 PathFileExistsW TRUE for a file the guest created itself
//   0x100 PathFileExistsW FALSE for a missing file
//   0x200 PathIsDirectoryW: TRUE dir, FALSE file
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) u16* GSTD PathCombineW(u16* dst, const u16* dir,
                                                   const u16* file);
__attribute__((dllimport)) const u16* GSTD PathFindFileNameW(const u16* path);
__attribute__((dllimport)) u16* GSTD PathAppendW(u16* path, const u16* more);
__attribute__((dllimport)) void GSTD PathStripPathW(u16* path);
__attribute__((dllimport)) const u16* GSTD PathFindExtensionW(const u16* path);
__attribute__((dllimport)) void GSTD PathRemoveExtensionW(u16* path);
__attribute__((dllimport)) i32 GSTD PathIsRelativeW(const u16* path);
__attribute__((dllimport)) i32 GSTD PathFileExistsW(const u16* path);
__attribute__((dllimport)) i32 GSTD PathIsDirectoryW(const u16* path);

__attribute__((dllimport)) void* GSTD CreateFileW(const u16* name,
                                                  u32 access, u32 share,
                                                  void* sa, u32 disp,
                                                  u32 flags, void* tmpl);
__attribute__((dllimport)) i32 GSTD CloseHandle(void* h);

static u32 weq(const u16* a, const char* b) {
  u32 i = 0;
  while (b[i] && a[i] == (u16)(u8)b[i]) ++i;
  return b[i] == 0 && a[i] == 0;
}

u32 GSTD guest_shlwapi_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. combine ----
  {
    static const u16 dir[10] = {'C', ':', '\\', 'd', 'i', 'r', 0};
    static const u16 file[9] = {'f', 'i', 'l', 'e', '.', 't', 'x', 't', 0};
    u16 out[260];
    if (PathCombineW(out, dir, file) && weq(out, "C:\\dir\\file.txt"))
      bits |= 0x01u;
  }

  // ---- 2. find file name ----
  {
    static const u16 p[24] = {'C',  ':',  '\\', 'g', 'a', 'm', 'e',
                              '\\', 'b', 'i', 'n', '\\', 'e', 'n',
                              'g',  'i', 'n', 'e', '.', 'd', 'l', 'l', 0};
    const u16* fn = PathFindFileNameW(p);
    if (fn && weq(fn, "engine.dll")) bits |= 0x02u;
  }

  // ---- 3. append ----
  {
    u16 p[260];
    static const u16 base[6] = {'C', ':', '\\', 's', 'v', 0};
    static const u16 more[7] = {'c', 'o', 'n', 'f', 'i', 'g', 0};
    u32 i = 0;
    for (; base[i]; ++i) p[i] = base[i];
    p[i] = 0;
    if (PathAppendW(p, more) && weq(p, "C:\\sv\\config")) bits |= 0x04u;
  }

  // ---- 4. strip path ----
  {
    u16 p[20];
    static const u16 src[14] = {'C', ':',  '\\', 'x', '\\', 'd', 'a',
                                't', 'a',  '.',  'b', 'i',  'n', 0};
    u32 i = 0;
    for (; src[i]; ++i) p[i] = src[i];
    p[i] = 0;
    PathStripPathW(p);
    if (weq(p, "data.bin")) bits |= 0x08u;
  }

  // ---- 5/6. extension find + remove ----
  {
    static const u16 p[12] = {'s', 'e', 't', 't', 'i', 'n',
                              'g', 's', '.',  'c', 'f', 0};
    static u16 buf[12];
    u32 i = 0;
    for (; p[i]; ++i) buf[i] = p[i];
    buf[i] = 0;
    const u16* ext = PathFindExtensionW(buf);
    if (ext && ext[0] == '.' && ext[1] == 'c') bits |= 0x10u;
    PathRemoveExtensionW(buf);
    if (weq(buf, "settings")) bits |= 0x20u;
  }

  // ---- 7. relative vs absolute ----
  {
    static const u16 abs[6] = {'C', ':', '\\', 'x', 'y', 0};
    static const u16 rel[4] = {'x', '\\', 'y', 0};
    if (!PathIsRelativeW(abs) && PathIsRelativeW(rel)) bits |= 0x40u;
  }

  // ---- 8/9. existence: real file vs missing ----
  {
    static const u16 name[13] = {'C', ':', '\\', 'e', 'x', 'i', 's',
                                 't', 's', '.',  'b', 'i', 'n', 0};
    void* f = CreateFileW(name, 0x40000000 /*GENERIC_WRITE*/, 0, 0,
                          2 /*CREATE_ALWAYS*/, 0, 0);
    if (f && f != (void*)-1) {
      CloseHandle(f);
      if (PathFileExistsW(name)) bits |= 0x80u;
      if (!PathIsDirectoryW(name)) bits |= 0x200u;
    }
    static const u16 missing[11] = {'C', ':', '\\', 'n', 'o', 'p',
                                    'e', '.',  'o',  'g', 0};
    if (!PathFileExistsW(missing)) bits |= 0x100u;
  }

  return bits;
}
