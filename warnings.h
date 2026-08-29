#pragma once
// Note: this is a boost-free version of contrib/epee/include/warnings.h.
// Only for compiler warning-suppression pragmas - unrelated to the algorithm.
#define PUSH_WARNINGS _Pragma("GCC diagnostic push")
#define POP_WARNINGS _Pragma("GCC diagnostic pop")
#define DISABLE_VS_WARNINGS(w)
#define DISABLE_GCC_AND_CLANG_WARNING(w)
#define DISABLE_GCC_WARNING(w)
#define DISABLE_CLANG_WARNING(w)