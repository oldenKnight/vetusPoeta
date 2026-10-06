/* Compiles the vendored miniz (third_party/miniz.c) into vp_core, once. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#if defined(_MSC_VER)
/* Vendored code, not ours: C4132 flags miniz's tentative definition of the const table
   s_tdefl_num_probes (defined further down). */
#pragma warning(push)
#pragma warning(disable : 4132)
#endif
/* The root CMake defines WIN32_LEAN_AND_MEAN for MSVC and miniz defines it again before
   including <windows.h> (C4005 macro redefinition); let miniz define it itself. */
#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#include "miniz.c"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
