// tiramisu/src/trt/bridge_wldap32.cpp
// WLDAP32.dll runtime bridge / fallback surface.
// Provides honest failure stubs (LDAP_SERVER_DOWN / NULL) for the 16 ordinals
// and symbols imported by Sekiro: Shadows Die Twice.
// Registered with route TRT_ROUTE_SYSTEM so that on console the OS's real
// wldap32.dll is preferred if present, falling back to these stubs.
#include "bridge_wldap32.h"
#include "trt.h"
#include <stdio.h>
#include <string.h>

extern "C" {

static const char* k_err_str = "LDAP server down";

const char* TRT_GUEST_ABI trt_ldap_err2string(int err) {
  (void)err;
  return k_err_str;
}

void* TRT_GUEST_ABI trt_ldap_first_entry(void* ld, void* res) {
  (void)ld; (void)res;
  return nullptr;
}

void* TRT_GUEST_ABI trt_ldap_next_entry(void* ld, void* entry) {
  (void)ld; (void)entry;
  return nullptr;
}

void* TRT_GUEST_ABI trt_ldap_get_dn(void* ld, void* entry) {
  (void)ld; (void)entry;
  return nullptr;
}

void* TRT_GUEST_ABI trt_ldap_first_attribute(void* ld, void* entry, void** ptr) {
  (void)ld; (void)entry;
  if (ptr) *ptr = nullptr;
  return nullptr;
}

void* TRT_GUEST_ABI trt_ldap_next_attribute(void* ld, void* entry, void* ptr) {
  (void)ld; (void)entry; (void)ptr;
  return nullptr;
}

void* TRT_GUEST_ABI trt_ldap_get_values_len(void* ld, void* entry, const void* target) {
  (void)ld; (void)entry; (void)target;
  return nullptr;
}

u32 TRT_GUEST_ABI trt_ldap_msgfree(void* lm) {
  (void)lm;
  return 0;
}

u32 TRT_GUEST_ABI trt_ldap_unbind_s(void* ld) {
  (void)ld;
  return 0;
}

u32 TRT_GUEST_ABI trt_ldap_search_s(void* ld, const void* base, int scope,
                                    const void* filter, void* attrs,
                                    int attrsonly, void** res) {
  (void)ld; (void)base; (void)scope; (void)filter; (void)attrs; (void)attrsonly;
  if (res) *res = nullptr;
  return 0x51; // LDAP_SERVER_DOWN
}

u32 TRT_GUEST_ABI trt_ldap_simple_bind_s(void* ld, const void* dn, const void* passwd) {
  (void)ld; (void)dn; (void)passwd;
  return 0x51; // LDAP_SERVER_DOWN
}

void TRT_GUEST_ABI trt_ldap_value_free_len(void* vals) {
  (void)vals;
}

void* TRT_GUEST_ABI trt_ldap_init(const void* host, u32 port) {
  (void)host; (void)port;
  return nullptr;
}

void TRT_GUEST_ABI trt_ldap_memfree(void* block) {
  (void)block;
}

u32 TRT_GUEST_ABI trt_ldap_set_option(void* ld, int option, const void* invalue) {
  (void)ld; (void)option; (void)invalue;
  return 0x51; // LDAP_SERVER_DOWN
}

void TRT_GUEST_ABI trt_ber_free(void* ber, int freebuf) {
  (void)ber; (void)freebuf;
}

int trt_bridge_register_wldap32(void) {
  static const trt_named_fn k_fns[] = {
      {"#22", (void*)&trt_ldap_err2string},
      {"ldap_err2string", (void*)&trt_ldap_err2string},
      {"#26", (void*)&trt_ldap_first_entry},
      {"ldap_first_entry", (void*)&trt_ldap_first_entry},
      {"#27", (void*)&trt_ldap_next_entry},
      {"ldap_next_entry", (void*)&trt_ldap_next_entry},
      {"#30", (void*)&trt_ldap_get_dn},
      {"ldap_get_dn", (void*)&trt_ldap_get_dn},
      {"#32", (void*)&trt_ldap_first_attribute},
      {"ldap_first_attribute", (void*)&trt_ldap_first_attribute},
      {"#33", (void*)&trt_ldap_next_attribute},
      {"ldap_next_attribute", (void*)&trt_ldap_next_attribute},
      {"#35", (void*)&trt_ldap_get_values_len},
      {"ldap_get_values_len", (void*)&trt_ldap_get_values_len},
      {"#41", (void*)&trt_ldap_msgfree},
      {"ldap_msgfree", (void*)&trt_ldap_msgfree},
      {"#46", (void*)&trt_ldap_unbind_s},
      {"ldap_unbind_s", (void*)&trt_ldap_unbind_s},
      {"#50", (void*)&trt_ldap_search_s},
      {"ldap_search_s", (void*)&trt_ldap_search_s},
      {"#60", (void*)&trt_ldap_simple_bind_s},
      {"ldap_simple_bind_s", (void*)&trt_ldap_simple_bind_s},
      {"#79", (void*)&trt_ldap_value_free_len},
      {"ldap_value_free_len", (void*)&trt_ldap_value_free_len},
      {"#143", (void*)&trt_ldap_init},
      {"ldap_init", (void*)&trt_ldap_init},
      {"#200", (void*)&trt_ldap_memfree},
      {"ldap_memfree", (void*)&trt_ldap_memfree},
      {"#211", (void*)&trt_ldap_set_option},
      {"ldap_set_option", (void*)&trt_ldap_set_option},
      {"#301", (void*)&trt_ber_free},
      {"ber_free", (void*)&trt_ber_free},
  };
  trt_module_register("wldap32.dll", TRT_ROUTE_SYSTEM, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
