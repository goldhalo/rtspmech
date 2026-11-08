// =============================================================================
//  rtspmech/export.hpp —— 共享库导出宏
// =============================================================================
//  cmake -DRTSPMECH_LIBRARY_TYPE=SHARED：
//    · 库本身定义 RTSPMECH_BUILDING_LIBRARY → dllexport
//    · 使用方定义 RTSPMECH_SHARED → dllimport（Windows）
//  Linux 配合 CXX_VISIBILITY_PRESET=hidden，只导出标了 RTSPMECH_API 的符号。
//  STATIC（默认）展开为空。
// =============================================================================
#pragma once

#if defined(_WIN32) && defined(RTSPMECH_SHARED)
#  if defined(RTSPMECH_BUILDING_LIBRARY)
#    define RTSPMECH_API __declspec(dllexport)
#  else
#    define RTSPMECH_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(RTSPMECH_SHARED)
#  define RTSPMECH_API __attribute__((visibility("default")))
#else
#  define RTSPMECH_API
#endif
