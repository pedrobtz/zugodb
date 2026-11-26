#pragma once

#include "zugodb/common/exception.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace zugodb {

// zugodb: one process-wide cap on the engine's threads, 2 unless the R
// glue sets it from dbConnect(threads = ) (design D5), so a check on CRAN's
// two cores is not oversubscribed. Every pool size and parallel split reads
// it through HWThreads(); upstream's ZUGODB_MAX_THREADS is not read.
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

// ---------------------------------------------------------------------------
// Errors on worker threads (libzugodb Stage 1).
//
// An exception that leaves a std::thread's function calls std::terminate,
// and one that leaves the calling thread while workers are still joinable
// does too (~thread). So every body that runs on an engine worker goes
// through WorkerErrors::Run, which keeps the first exception, and the
// query's thread rethrows it after the join: the error reaches
// Connection::Query like any other. A new thread start in src/ uses
// WorkerGroup (fork and join in one scope), ParallelFor, or, for workers
// that outlive the call that starts them, WorkerErrors directly with a
// rethrow on the consuming thread once the workers are joined.
// ---------------------------------------------------------------------------

// The kinds of thread start in src/, for the test seam below.
enum class WorkerSite : unsigned int {
    NONE = 0,
    FORK_JOIN = 1,    // WorkerGroup: threads started and joined in one scope
    PARALLEL_FOR = 2, // ParallelFor
    POOL = 4,         // tasks on ThreadPool (ParallelExecutor)
};

// Test seam. While a site kind is armed, every worker body of that kind
// throws an InternalException before it runs, and the hit is counted. It
// costs one relaxed atomic load per worker body (not per row), and nothing
// arms it outside the tests (test/unit/execution/test_worker_errors.cpp).
inline std::atomic<unsigned int> &WorkerFaultSiteRef() {
    static std::atomic<unsigned int> site{0};
    return site;
}
inline std::atomic<unsigned long long> &WorkerFaultHitsRef() {
    static std::atomic<unsigned long long> hits{0};
    return hits;
}
inline void ArmWorkerFault(WorkerSite site) {
    WorkerFaultHitsRef().store(0);
    WorkerFaultSiteRef().store(static_cast<unsigned int>(site));
}
inline void DisarmWorkerFault() { WorkerFaultSiteRef().store(0); }
inline unsigned long long WorkerFaultHits() { return WorkerFaultHitsRef().load(); }
inline void WorkerFaultPoint(WorkerSite site) {
    if (WorkerFaultSiteRef().load(std::memory_order_relaxed) ==
        static_cast<unsigned int>(site)) {
        WorkerFaultHitsRef().fetch_add(1);
        throw InternalException("worker fault injected by the test seam");
    }
}

// The first exception of a set of workers. Thread-safe; Failed() is cheap
// enough to test in a worker's loop so the others stop early.
class WorkerErrors {
public:
    // Run `fn()` unless a worker has already failed; keep what it throws.
    template <class Fn>
    void Run(WorkerSite site, Fn &&fn) noexcept {
        if (Failed()) return;
        try {
            WorkerFaultPoint(site);
            fn();
        } catch (...) {
            Capture(std::current_exception());
        }
    }

    void Capture(std::exception_ptr e) noexcept {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!first_) first_ = std::move(e);
        }
        failed_.store(true, std::memory_order_release);
    }

    bool Failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    // Call after every worker is joined: rethrows the first exception, if
    // any, and leaves this empty for the next run.
    void Rethrow() {
        if (!Failed()) return;
        std::exception_ptr e;
        {
            std::lock_guard<std::mutex> lk(mu_);
            e = std::move(first_);
            first_ = nullptr;
        }
        failed_.store(false, std::memory_order_release);
        if (e) std::rethrow_exception(e);
    }

    void Reset() noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        first_ = nullptr;
        failed_.store(false, std::memory_order_release);
    }

private:
    std::mutex mu_;
    std::exception_ptr first_;
    std::atomic<bool> failed_{false};
};

// Threads started and joined in one scope. Replaces
//
//   std::vector<std::thread> ts;
//   for (...) ts.emplace_back(fn, args...);
//   fn(0);                         // optional share of the calling thread
//   for (auto &t : ts) t.join();
//
// with Spawn / RunHere / Join. Join() rethrows the first exception of any
// body, the calling thread's included, after every thread has ended. A
// thread that cannot be started is an error of the group, not an
// exception that unwinds the frame running workers still use. The
// destructor joins whatever Join() did not (an exception between Spawn and
// Join), so no joinable std::thread is ever destroyed.
class WorkerGroup {
public:
    explicit WorkerGroup(WorkerSite site = WorkerSite::FORK_JOIN) : site_(site) {}
    ~WorkerGroup() { JoinAll(); }
    WorkerGroup(const WorkerGroup &) = delete;
    WorkerGroup &operator=(const WorkerGroup &) = delete;

    void reserve(std::size_t n) { threads_.reserve(n); }

    template <class Fn, class... Args>
    void Spawn(Fn &&fn, Args &&...args) {
        if (errors_.Failed()) return;
        try {
            threads_.emplace_back(
                [this](auto f, auto... a) { errors_.Run(site_, [&] { f(a...); }); },
                std::forward<Fn>(fn), std::forward<Args>(args)...);
        } catch (...) {
            errors_.Capture(std::current_exception());
        }
    }

    // Run a share of the work on the calling thread, under the same capture.
    template <class Fn, class... Args>
    void RunHere(Fn &&fn, Args &&...args) {
        errors_.Run(site_, [&] { fn(std::forward<Args>(args)...); });
    }

    // Join every thread, then rethrow the first exception, if any.
    void Join() {
        JoinAll();
        errors_.Rethrow();
    }

    bool Failed() const noexcept { return errors_.Failed(); }

private:
    void JoinAll() noexcept {
        for (auto &t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
    }

    WorkerSite site_;
    WorkerErrors errors_;
    std::vector<std::thread> threads_;
};

// Run `fn(t)` for `t` in `[0, nt)`. When `nt > 1` and threading is
// available, spawn `nt - 1` worker threads and run `fn(0)` on the
// calling thread; otherwise run `fn(0)` inline. Joins all workers
// before returning, then rethrows the first exception of any `fn(t)`.
// Mirrors the spawn-and-join pattern that physical
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
    WorkerGroup group(WorkerSite::PARALLEL_FOR);
    group.reserve(nt - 1);
    for (unsigned int t = 1; t < nt; t++) group.Spawn(fn, t);
    group.RunHere(fn, 0u);
    group.Join();
#endif
}

} // namespace zugodb
