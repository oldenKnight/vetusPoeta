// CPUID gate (DESIGN §11, D4). The vendored ggml-cpu is compiled for AVX2 + FMA + F16C + BMI2 (cmake/vp_llama.cmake)
// and executes such instructions as soon as the backend initialises, so this check runs before any llama.cpp call.
// It needs no llama.cpp header and uses only the compiler's CPUID intrinsics.
#include "vp/llm.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <immintrin.h>
#define VP_CPUID_MSVC 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#define VP_CPUID_GCC 1
#endif

namespace vp::llm {

namespace {

struct Regs {
  unsigned a = 0, b = 0, c = 0, d = 0;
};

#if defined(VP_CPUID_MSVC)
unsigned maxLeaf() {
  int r[4] = {0, 0, 0, 0};
  __cpuid(r, 0);
  return static_cast<unsigned>(r[0]);
}
Regs leaf(unsigned l, unsigned sub) {
  int r[4] = {0, 0, 0, 0};
  __cpuidex(r, static_cast<int>(l), static_cast<int>(sub));
  return Regs{static_cast<unsigned>(r[0]), static_cast<unsigned>(r[1]), static_cast<unsigned>(r[2]),
              static_cast<unsigned>(r[3])};
}
unsigned long long xcr0() { return _xgetbv(0); }
#elif defined(VP_CPUID_GCC)
unsigned maxLeaf() { return __get_cpuid_max(0, nullptr); }
Regs leaf(unsigned l, unsigned sub) {
  Regs r;
  if (!__get_cpuid_count(l, sub, &r.a, &r.b, &r.c, &r.d)) return Regs{};
  return r;
}
unsigned long long xcr0() {
  unsigned lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));   // only called when OSXSAVE is set
  return (static_cast<unsigned long long>(hi) << 32) | lo;
}
#endif

CpuFeatures probe() {
  CpuFeatures f;
#if defined(VP_CPUID_MSVC) || defined(VP_CPUID_GCC)
  f.x86 = true;
  const unsigned top = maxLeaf();
  if (top < 1) return f;
  const Regs l1 = leaf(1, 0);
  const bool osxsave = (l1.c >> 27) & 1u;
  f.avx = (l1.c >> 28) & 1u;
  f.fma = (l1.c >> 12) & 1u;
  f.f16c = (l1.c >> 29) & 1u;
  // The OS must save the XMM and YMM state (XCR0 bits 1 and 2), otherwise AVX instructions fault.
  f.osAvx = osxsave && ((xcr0() & 0x6ull) == 0x6ull);
  if (top >= 7) {
    const Regs l7 = leaf(7, 0);
    f.avx2 = (l7.b >> 5) & 1u;
    f.bmi2 = (l7.b >> 8) & 1u;
  }
#endif
  return f;
}

}  // namespace

CpuFeatures cpuFeatures() {
  static const CpuFeatures cached = probe();
  return cached;
}

bool cpuSupported() {
  const CpuFeatures f = cpuFeatures();
  return f.x86 && f.osAvx && f.avx && f.avx2 && f.fma && f.f16c && f.bmi2;
}

}  // namespace vp::llm
