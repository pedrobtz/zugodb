#pragma once

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

namespace zugodb {

// zuslothdb: one process-wide cap on the engine's threads, 2 unless the R
// glue sets it from dbConnect(threads = ) (design D5), so a check on CRAN's
// two cores is not oversubscribed. Every pool size and parallel split reads
// it through HWThreads(); upstream's SLOTHDB_MAX_THREADS is not read.
inline std::atomic<unsigned int> &ThreadCapRef() {
    static std::atomic<unsigned int> cap{2};
    return cap;
}
inline unsigned int ThreadCap() { return ThreadCapRef().load(); }
inline void SetThreadCap(unsigned int n) { ThreadCapRef().store(n == 0 ? 1 : n); }

// Returns the number of hardware threads available, clamped to 1 on
// single-threaded WASM (Emscripten without pthreads). Std::thread's
// constructor throws "thread constructor failed: Not supported" in
// that environment, so callers must check the return value before
// spawning. With this helper, callers can uniformly do:
//
//   unsigned int nt = zugodb::HWThreads();
//   if (nt > 1) { ...spawn... } else { ...inline... }
inline unsigned int HWThreads() {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return 1;
#else
    unsigned int n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    unsigned int cap = ThreadCap();
    if (cap < n) n = cap;
    return n;
#endif
}

// Run `fn(t)` for `t` in `[0, nt)`. When `nt > 1` and threading is
// available, spawn `nt - 1` worker threads and run `fn(0)` on the
// calling thread; otherwise run `fn(0)` inline. Joins all workers
// before returning. Mirrors the spawn-and-join pattern that physical
// operators were repeating, with an inline fallback that's safe on
// single-threaded WASM.
template <class Fn>
inline void ParallelFor(unsigned int nt, Fn &&fn) {
    if (nt <= 1) {
        fn(0u);
        return;
    }
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    for (unsigned int t = 0; t < nt; t++) fn(t);
#else
    std::vector<std::thread> ts;
    ts.reserve(nt - 1);
    for (unsigned int t = 1; t < nt; t++) ts.emplace_back(fn, t);
    fn(0u);
    for (auto &t : ts) if (t.joinable()) t.join();
#endif
}

} // namespace zugodb
