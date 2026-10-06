/* Force-included (MinGW only, ggml-cpu.c only) when the MinGW-w64 headers lack THREAD_POWER_THROTTLING_STATE
 * (mingw-w64 v11 has the ThreadPowerThrottling enum value but not the struct). Layout per the Windows SDK
 * (processthreadsapi.h): three ULONGs; ULONG is 32-bit unsigned long on LLP64 Windows. See cmake/vp_llama.cmake. */
#ifndef VP_MINGW_THREAD_POWER_H
#define VP_MINGW_THREAD_POWER_H
typedef struct _THREAD_POWER_THROTTLING_STATE {
  unsigned long Version;
  unsigned long ControlMask;
  unsigned long StateMask;
} THREAD_POWER_THROTTLING_STATE;
#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1
#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
