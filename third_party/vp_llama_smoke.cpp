// Link and init check for the vendored llama.cpp (target vp_llama). No model is loaded and nothing touches the
// network: it initialises the backend, requires the CPU device to be registered, prints the compiled CPU features
// and frees the backend. Built as `vp_llama_smoke` when VP_WITH_LLM and VP_BUILD_TESTS are ON (ctest: vp_llama_smoke).
#include <cstdio>
#include <cstring>

#include "ggml-backend.h"
#include "llama.h"

namespace {
void quietLog(enum ggml_log_level, const char*, void*) {}
}  // namespace

int main() {
  llama_log_set(quietLog, nullptr);
  llama_backend_init();
  int rc = 0;
  const size_t devices = ggml_backend_dev_count();
  bool haveCpu = false;
  for (size_t i = 0; i < devices; ++i) {
    const char* name = ggml_backend_dev_name(ggml_backend_dev_get(i));
    if (name != nullptr && std::strcmp(name, "CPU") == 0) haveCpu = true;
  }
  if (!haveCpu) {
    std::fprintf(stderr, "vp_llama_smoke: CPU backend not registered (%zu devices)\n", devices);
    rc = 1;
  }
  if (!llama_supports_mmap()) {
    std::fprintf(stderr, "vp_llama_smoke: mmap not supported\n");
    rc = 1;
  }
  std::printf("vp_llama_smoke: %zu device(s); %s\n", devices, llama_print_system_info());
  llama_backend_free();
  return rc;
}
