// [auditA] Native stack bound for the evaluator / parser recursion guards (mxs_internal.h
// stack_exhausted()): deep script recursion or pathological nesting raises a MAXScript error
// ("Stack overflow") instead of crashing the host. Kept in its own file so the platform headers
// (windows.h / pthread.h) stay out of the interpreter sources.
#include "mxs_internal.h"
#include <algorithm>
#include <cstdint>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#else
#  include <pthread.h>
#endif

namespace mxs {

uintptr_t stack_limit_for_thread() {
    uintptr_t lo = 0, hi = 0;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(&mbi, &mbi, sizeof mbi)) {
        lo = (uintptr_t)mbi.AllocationBase;   // reserve base of this thread's stack
        hi = (uintptr_t)&mbi;                 // (a point near the top: the size below is conservative)
    }
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    hi = (uintptr_t)pthread_get_stackaddr_np(self);
    lo = hi - (uintptr_t)pthread_get_stacksize_np(self);
#else
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&a, &addr, &size) == 0 && addr && size) { lo = (uintptr_t)addr; hi = lo + size; }
        pthread_attr_destroy(&a);
    }
#endif
    if (!lo || hi <= lo) return 0;   // unknown: no guard
    // keep a reserve for natives called from the deepest script frame, unwinding and the host
    const uintptr_t size = hi - lo;
    const uintptr_t reserve = std::min<uintptr_t>((uintptr_t)256 * 1024, size / 4);
    return lo + reserve;
}

}  // namespace mxs
