// tiramisu/src/trt/bridge_crypt32.cpp
// crypt32 bridge. See bridge_crypt32.h.
//
// No certificate store on console. Open verbs fail with ERROR-class codes
// and name the reason; Free/Close verbs succeed (freeing nothing is not a
// lie). UnityPlayer probes the cert surface at boot and an honest FALSE
// sends it down the no-cert path.
#include "bridge_crypt32.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {
constexpr u32 k_err_invalid_parameter = 87;
constexpr u32 k_err_no_data = 232;

namespace cryptlog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) cryptlog::line(s_)
}  // namespace

extern "C" {

void* TRT_GUEST_ABI crypt_CertOpenSystemStoreW(void* hprov, const u16* name) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertOpenSystemStoreW -> NULL (no cert store on console; "
           "store=%s)", name ? "named" : "default");
  log_line(line);
  return nullptr;
}

void* TRT_GUEST_ABI crypt_CertOpenSystemStoreA(void* hprov, const char* name) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertOpenSystemStoreA -> NULL (no cert store on console; "
           "store=%s)", name ? name : "default");
  log_line(line);
  return nullptr;
}

void* TRT_GUEST_ABI crypt_CertOpenStore(const void* provider, u32 encoding,
                                        void* hprov, u32 flags, const void* para) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertOpenStore -> NULL (no cert store on console)");
  log_line(line);
  (void)provider; (void)encoding; (void)hprov; (void)flags; (void)para;
  return nullptr;
}

int TRT_GUEST_ABI crypt_CertCloseStore(void* store, u32 flags) {
  char line[128];
  snprintf(line, sizeof(line),
           "crypt32: CertCloseStore -> TRUE (no store was ever open)");
  log_line(line);
  (void)store; (void)flags;
  return 1;
}

int TRT_GUEST_ABI crypt_CertAddEncodedCertificateToStore(void* store,
                                                         u32 encoding,
                                                         const u8* cert,
                                                         u32 clen, u32 add,
                                                         void** ctx) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertAddEncodedCertificateToStore -> FALSE (no cert store "
           "on console)");
  log_line(line);
  (void)store; (void)encoding; (void)cert; (void)clen; (void)add;
  if (ctx) *ctx = nullptr;
  return 0;
}

void* TRT_GUEST_ABI crypt_CertGetCertificateChain(void* engine, void* leaf,
                                                  void* time, void* store,
                                                  void* para, u32 flags,
                                                  void* rsv) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertGetCertificateChain -> FALSE (no cert engine on "
           "console)");
  log_line(line);
  (void)engine; (void)leaf; (void)time; (void)store; (void)para; (void)flags;
  (void)rsv;
  return nullptr;
}

int TRT_GUEST_ABI crypt_CertVerifyCertificateChainPolicy(void* policy,
                                                         void* chain,
                                                         void* para,
                                                         void* status) {
  char line[160];
  snprintf(line, sizeof(line),
           "crypt32: CertVerifyCertificateChainPolicy -> FALSE (no chain was "
           "built: no cert engine)");
  log_line(line);
  (void)policy; (void)chain; (void)para; (void)status;
  return 0;
}

void TRT_GUEST_ABI crypt_CertFreeCertificateContext(void* ctx) {
  char line[128];
  snprintf(line, sizeof(line),
           "crypt32: CertFreeCertificateContext -> freed (nothing allocated)");
  log_line(line);
  (void)ctx;
}

void TRT_GUEST_ABI crypt_CertFreeCertificateChain(void* chain) {
  char line[128];
  snprintf(line, sizeof(line),
           "crypt32: CertFreeCertificateChain -> freed (nothing allocated)");
  log_line(line);
  (void)chain;
}

int trt_bridge_register_crypt32(void) {
  static const trt_named_fn k_fns[] = {
      {"CertOpenSystemStoreW", (void*)&crypt_CertOpenSystemStoreW},
      {"CertOpenSystemStoreA", (void*)&crypt_CertOpenSystemStoreA},
      {"CertOpenStore", (void*)&crypt_CertOpenStore},
      {"CertCloseStore", (void*)&crypt_CertCloseStore},
      {"CertAddEncodedCertificateToStore",
       (void*)&crypt_CertAddEncodedCertificateToStore},
      {"CertGetCertificateChain", (void*)&crypt_CertGetCertificateChain},
      {"CertVerifyCertificateChainPolicy",
       (void*)&crypt_CertVerifyCertificateChainPolicy},
      {"CertFreeCertificateContext", (void*)&crypt_CertFreeCertificateContext},
      {"CertFreeCertificateChain", (void*)&crypt_CertFreeCertificateChain},
  };
  trt_module_register("crypt32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
