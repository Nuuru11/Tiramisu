// src/guest/guest_res.c — the guest's code section: references the .rsrc
// object's symbol so the linker keeps the section, then returns.
#ifdef __cplusplus
extern "C" {
#endif
extern char g_rsrc[];
#ifdef __cplusplus
}
#endif
void guest_res_boot(void) {
  // touch one byte so the object is not dead-stripped
  volatile char c = g_rsrc[0];
  (void)c;
}
