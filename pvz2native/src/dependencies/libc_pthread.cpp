/* libc.so -- <pthread.h>, <semaphore.h>, <sched.h>. On bionic these live in
 * libc itself rather than a separate libpthread.
 *
 * Every guest thread is a real host thread driving its own dynarmic Jit over
 * the shared address space, so the synchronisation primitives are real too: a
 * guest pthread_mutex_t address is the key to a host std::mutex (GuestMutex),
 * a pthread_cond_t address maps to a condition_variable, and so on. Nothing is
 * faked into "always succeeds", because the engine's loader genuinely depends
 * on one thread blocking until another signals.
 *
 * Blocking waits report themselves if they run long instead of hanging in
 * silence. A guest thread stuck in a lock never returns from jit.Run(), so on
 * the main thread the host pump stops, the window goes "Not responding", and
 * nothing in the log says why -- which is exactly how an early deadlock in
 * GameAppInitialize presented.
 */

#include <pvz2native/dependencies/dependency.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace pvz2native {
namespace {

using namespace std::chrono_literals;

constexpr auto kStuckWarnAfter = 3000ms;
constexpr std::uint32_t kEBUSY = 16;
constexpr std::uint32_t kEAGAIN = 11;
constexpr std::uint32_t kETIMEDOUT = 110;

/* Absolute deadlines in the guest's timespec are CLOCK_REALTIME seconds+nanos,
 * which is the same epoch the host's system_clock uses. */
std::chrono::system_clock::time_point deadline_from(GuestCall &c, std::uint32_t ts) {
    std::uint32_t sec = c.read32(ts), nsec = c.read32(ts + 4);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(sec) + std::chrono::nanoseconds(nsec)));
}

/* ---------------------------------------------------------------- mutex
 *
 * The mutex TYPE has to survive the crossing, because a recursive mutex that
 * loses its recursiveness deadlocks the moment its owner relocks it -- and the
 * engine does exactly that. bionic records the type in two places, and both are
 * honoured here:
 *   - pthread_mutexattr_t is a plain word holding the type, which
 *     pthread_mutex_init then reads;
 *   - the pthread_mutex_t's own value word carries it in bits 14-15
 *     (0x4000 = recursive), which is what makes a statically initialised
 *     PTHREAD_RECURSIVE_MUTEX_INITIALIZER work with no init call at all. */
constexpr std::uint32_t kMutexTypeMask = 0xC000;
constexpr std::uint32_t kMutexRecursiveBits = 0x4000;
constexpr std::uint32_t kPthreadMutexRecursive = 1;

GuestMutex *mutex_for(GuestCall &c, std::uint32_t addr) {
    GuestMutex *gm = c.rt->get_or_create_mutex(addr);
    if (!gm->type_known) {
        gm->set_recursive((c.read32(addr) & kMutexTypeMask) == kMutexRecursiveBits);
    }
    return gm;
}

void c_mutex_init(GuestCall &c) {
    GuestMutex *gm = c.rt->get_or_create_mutex(c.arg(0));
    std::uint32_t attr = c.arg(1);
    std::uint32_t type = (attr != 0) ? (c.read32(attr) & 0xFu) : 0u;
    gm->set_recursive(type == kPthreadMutexRecursive);
    c.set_result(0);
}

void c_mutexattr_init(GuestCall &c) {
    if (c.arg(0) != 0) c.write32(c.arg(0), 0); /* PTHREAD_MUTEX_NORMAL */
    c.set_result(0);
}

/* Stores the type in the attribute object itself, exactly as bionic does, so
 * pthread_mutex_init can read it back without any host-side bookkeeping. */
void c_mutexattr_settype(GuestCall &c) {
    if (c.arg(0) != 0) c.write32(c.arg(0), c.arg(1));
    c.set_result(0);
}

void c_mutexattr_gettype(GuestCall &c) {
    if (c.arg(1) != 0) c.write32(c.arg(1), c.arg(0) != 0 ? (c.read32(c.arg(0)) & 0xFu) : 0u);
    c.set_result(0);
}

/* Keeps the object, like c_sem_destroy -- erasing it frees a host mutex that a
 * blocked guest thread is still holding a raw pointer to. The entry costs a
 * few bytes and the address space reuses these constantly, so the map is
 * bounded either way. */
void c_mutex_destroy(GuestCall &c) {
    c.rt->get_or_create_mutex(c.arg(0));
    c.set_result(0);
}

void c_mutex_lock(GuestCall &c) {
    c.note_blocked(); /* progress, not a spin -- keeps a worker off the runaway cap */
    std::uint32_t addr = c.arg(0);
    std::uint32_t self = guest_tls::self_id;
    GuestMutex *gm = mutex_for(c, addr);
    if (gm->would_self_deadlock(self)) {
        /* Waiting this out would hang forever with nothing to point at, and the
         * cause is always the same: a mutex the guest expects to be recursive
         * that we did not record as one. Say so once and take it anyway. */
        c.log("[deadlock] tid=%u relocked non-recursive mutex 0x%08x it already owns "
              "(lr=0x%08x) -- treating it as recursive", self, addr, c.lr());
        gm->set_recursive(true);
    }
    if (!gm->try_lock_for(kStuckWarnAfter, self)) {
        c.log("[stuck] tid=%u blocked >3s in pthread_mutex_lock(0x%08x) lr=0x%08x", self, addr,
              c.lr());
        gm->lock(self); /* semantics are unchanged: we still wait indefinitely */
        c.log("[stuck] tid=%u acquired mutex 0x%08x after waiting", self, addr);
    }
    c.set_result(0);
}

void c_mutex_unlock(GuestCall &c) {
    mutex_for(c, c.arg(0))->unlock(guest_tls::self_id);
    c.set_result(0);
}

void c_mutex_trylock(GuestCall &c) {
    c.set_result(mutex_for(c, c.arg(0))->try_lock(guest_tls::self_id) ? 0u : kEBUSY);
}

/* -------------------------------------------------- condition variables */

void c_cond_init(GuestCall &c) {
    c.rt->get_or_create_cond(c.arg(0));
    c.set_result(0);
}

/* Same reasoning as c_mutex_destroy: a thread parked in pthread_cond_wait
 * holds a pointer to this object, so it must outlive the guest's destroy. */
void c_cond_destroy(GuestCall &c) {
    c.rt->get_or_create_cond(c.arg(0));
    c.set_result(0);
}

void c_cond_signal(GuestCall &c) {
    GuestCond *gc = c.rt->get_or_create_cond(c.arg(0));
    { std::lock_guard<std::mutex> lk(gc->m); gc->generation++; }
    gc->cv.notify_one();
    c.set_result(0);
}

void c_cond_broadcast(GuestCall &c) {
    GuestCond *gc = c.rt->get_or_create_cond(c.arg(0));
    { std::lock_guard<std::mutex> lk(gc->m); gc->generation++; }
    gc->cv.notify_all();
    c.set_result(0);
}

void c_cond_wait(GuestCall &c) {
    c.note_blocked(); /* progress, not a spin -- keeps a worker off the runaway cap */
    GuestCond *gc = c.rt->get_or_create_cond(c.arg(0));
    GuestMutex *gm = mutex_for(c, c.arg(1));
    std::uint32_t self = guest_tls::self_id;
    std::unique_lock<std::mutex> lk(gc->m);
    /* The generation counter is what makes this immune to lost wakeups: a
     * signal that arrives between releasing the guest mutex and entering the
     * wait still bumps it, so the predicate is already true. */
    std::uint64_t gen = gc->generation;
    gm->unlock(self);
    if (!gc->cv.wait_for(lk, kStuckWarnAfter, [&] { return gc->generation != gen; })) {
        c.log("[stuck] tid=%u blocked >3s in pthread_cond_wait(cond=0x%08x, mutex=0x%08x) lr=0x%08x",
              self, c.arg(0), c.arg(1), c.lr());
        gc->cv.wait(lk, [&] { return gc->generation != gen; });
    }
    lk.unlock();
    gm->lock(self);
    c.set_result(0);
}

void c_cond_timedwait(GuestCall &c) {
    c.note_blocked(); /* progress, not a spin -- keeps a worker off the runaway cap */
    GuestCond *gc = c.rt->get_or_create_cond(c.arg(0));
    GuestMutex *gm = mutex_for(c, c.arg(1));
    std::uint32_t self = guest_tls::self_id;
    auto deadline = deadline_from(c, c.arg(2));
    std::unique_lock<std::mutex> lk(gc->m);
    std::uint64_t gen = gc->generation;
    gm->unlock(self);
    bool woke = gc->cv.wait_until(lk, deadline, [&] { return gc->generation != gen; });
    lk.unlock();
    gm->lock(self);
    c.set_result(woke ? 0u : kETIMEDOUT);
}

/* ----------------------------------------------------------- semaphores */

/* A [stuck] line names the address a thread is blocked on, but not whether the
 * wakeup it is waiting for was ever sent. This dumps every semaphore the guest
 * has touched so the two failure modes are told apart at a glance:
 *
 *   count > 0 while a thread is still parked here
 *       -> the post landed and WE lost the wakeup;
 *   posts == 0, or the last post came from an unexpected caller
 *       -> the post went somewhere else entirely (stale object, wrong
 *          address, a path that never reached this shim).
 *
 * Capped: all threads in a deadlock report themselves within the same second
 * and the table does not change afterwards. */
constexpr int kMaxSemReports = 3;
std::atomic<int> g_sem_reports{0};

void report_sems(GuestCall &c) {
    if (g_sem_reports.fetch_add(1, std::memory_order_relaxed) >= kMaxSemReports) return;
    std::lock_guard<std::mutex> lk(c.rt->sems_lock);
    c.log("[stuck] semaphore table (%u entries):", (unsigned)c.rt->guest_sems.size());
    for (const auto &entry : c.rt->guest_sems) {
        GuestSem *gs = entry.second.get();
        /* try_lock, never block: a thread parked in cv.wait has released this
         * mutex, so the only holder can be a brief update -- and a report that
         * can deadlock is worse than one with a hole in it. */
        std::unique_lock<std::mutex> slk(gs->m, std::try_to_lock);
        if (!slk) {
            c.log("[stuck]   0x%08x  <in use>", entry.first);
            continue;
        }
        if (gs->count == 0 && gs->waiters == 0 && gs->posts == 0) continue;
        c.log("[stuck]   0x%08x  count=%d waiters=%u posts=%u last_post=tid%u lr=0x%08x",
              entry.first, gs->count, gs->waiters, gs->posts, gs->last_post_tid,
              gs->last_post_lr);
    }
}

/* Reuses the GuestSem at this address instead of replacing it.
 *
 * Replacing it destroyed the host object out from under any guest thread
 * already blocked in sem_wait there: c_sem_wait holds a raw pointer to it and
 * sleeps on its condition_variable, so the waiter was left blocked on freed
 * memory while every later sem_post incremented a DIFFERENT object.
 *
 * That is exactly how audio deadlocked the game on PLAY. Wwise's bank thread
 * sits in sem_wait(bankmgr+40) waiting for work; CAkBankMgr::QueueBankCommand
 * posts that same address, and AK::SoundEngine::UnloadBank blocks the MAIN
 * thread on a stack semaphore until the bank thread answers. One orphaned
 * GuestSem and all three threads stop forever -- with the main thread stuck
 * inside onDrawFrame, which is why the window went "Not responding".
 *
 * pthread_mutex_init and pthread_cond_init already reused their objects; this
 * was the odd one out. */
void c_sem_init(GuestCall &c) {
    GuestSem *gs = c.rt->get_or_create_sem(c.arg(0));
    {
        std::lock_guard<std::mutex> lk(gs->m);
        gs->count = (int)c.arg(2);
    }
    gs->cv.notify_all(); /* a non-zero initial count can release a waiter */
    c.set_result(0);
}

/* Deliberately KEEPS the entry rather than erasing it: freeing the object
 * would strand any thread still blocked on it, the same way replacing it did.
 * Zeroing the count leaves it indistinguishable from a fresh semaphore, and
 * sem_init resets it on reuse -- which matters here because these live on the
 * guest STACK (UnloadBank creates one per call) and stack addresses are
 * recycled constantly. The map stays small for the same reason. */
void c_sem_destroy(GuestCall &c) {
    GuestSem *gs = c.rt->get_or_create_sem(c.arg(0));
    std::lock_guard<std::mutex> lk(gs->m);
    gs->count = 0;
    c.set_result(0);
}

void c_sem_wait(GuestCall &c) {
    c.note_blocked(); /* progress, not a spin -- keeps a worker off the runaway cap */
    GuestSem *gs = c.rt->get_or_create_sem(c.arg(0));
    std::unique_lock<std::mutex> lk(gs->m);
    gs->waiters++;
    if (!gs->cv.wait_for(lk, kStuckWarnAfter, [&] { return gs->count > 0; })) {
        /* Report with this semaphore's mutex released: the dump walks every
         * other semaphore and would otherwise hold two of them at once. */
        lk.unlock();
        c.log("[stuck] tid=%u blocked >3s in sem_wait(0x%08x) lr=0x%08x", guest_tls::self_id,
              c.arg(0), c.lr());
        report_sems(c);
        lk.lock();
        gs->cv.wait(lk, [&] { return gs->count > 0; });
    }
    gs->waiters--;
    gs->count--;
    c.set_result(0);
}

void c_sem_trywait(GuestCall &c) {
    GuestSem *gs = c.rt->get_or_create_sem(c.arg(0));
    std::lock_guard<std::mutex> lk(gs->m);
    if (gs->count > 0) {
        gs->count--;
        c.set_result(0);
        return;
    }
    c.set_errno(kEAGAIN);
    c.set_result((std::uint32_t)-1);
}

void c_sem_timedwait(GuestCall &c) {
    c.note_blocked(); /* progress, not a spin -- keeps a worker off the runaway cap */
    GuestSem *gs = c.rt->get_or_create_sem(c.arg(0));
    auto deadline = deadline_from(c, c.arg(1));
    std::unique_lock<std::mutex> lk(gs->m);
    if (gs->cv.wait_until(lk, deadline, [&] { return gs->count > 0; })) {
        gs->count--;
        c.set_result(0);
        return;
    }
    c.set_errno(kETIMEDOUT);
    c.set_result((std::uint32_t)-1);
}

void c_sem_post(GuestCall &c) {
    GuestSem *gs = c.rt->get_or_create_sem(c.arg(0));
    {
        std::lock_guard<std::mutex> lk(gs->m);
        gs->count++;
        gs->posts++;
        gs->last_post_tid = guest_tls::self_id;
        gs->last_post_lr = c.lr();
    }
    gs->cv.notify_one();
    c.set_result(0);
}

void c_sem_getvalue(GuestCall &c) {
    GuestSem *gs = c.rt->get_or_create_sem(c.arg(0));
    std::lock_guard<std::mutex> lk(gs->m);
    c.write32(c.arg(1), (std::uint32_t)gs->count);
    c.set_result(0);
}

/* --------------------------------------------------------------- once */

void c_once(GuestCall &c) {
    std::uint32_t once_addr = c.arg(0), init_routine = c.arg(1);
    bool need_call = false;
    {
        std::lock_guard<std::mutex> lk(c.rt->once_lock);
        bool &done = c.rt->once_done[once_addr];
        if (!done) { done = true; need_call = true; }
    }
    if (need_call) {
        /* The init routine must run on THIS thread -- it usually initialises
         * thread-local or singleton state the caller is about to use. */
        if (c.call_guest_fn != nullptr) {
            c.call_guest(init_routine, nullptr, 0);
        } else if (std::uint32_t id = c.spawn_thread(init_routine, 0)) {
            c.join_thread(id);
        }
    }
    c.set_result(0);
}

/* --------------------------------------------------------- threads / TLS */

void c_create(GuestCall &c) {
    std::uint32_t thread_out = c.arg(0), start_routine = c.arg(2), arg = c.arg(3);
    std::uint32_t id = c.spawn_thread(start_routine, arg);
    if (id != 0 && thread_out != 0) c.write32(thread_out, id);
    c.set_result(id != 0 ? 0u : kEAGAIN /* out of stack slots */);
}

void c_join(GuestCall &c) {
    std::uint32_t retval = c.join_thread(c.arg(0));
    if (c.arg(1) != 0) c.write32(c.arg(1), retval);
    c.set_result(0);
}

void c_detach(GuestCall &c) {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(c.rt->threads_lock);
        auto found = c.rt->threads.find(c.arg(0));
        if (found != c.rt->threads.end()) {
            th = std::move(found->second);
            c.rt->threads.erase(found);
        }
    }
    if (th.joinable()) th.detach();
    c.set_result(0);
}

void c_self(GuestCall &c) { c.set_result(guest_tls::self_id); }

void c_equal(GuestCall &c) { c.set_result(c.arg(0) == c.arg(1) ? 1u : 0u); }

void c_exit(GuestCall &c) {
    /* The thread terminates here: r0 is its return value and there is no
     * caller to resume, so the dispatcher must NOT restore pc from lr. */
    c.halt("pthread_exit");
}

void c_key_create(GuestCall &c) {
    /* Keys are process-wide; only the VALUES are per thread. Racy across
     * threads but monotonic, so two simultaneous creates cannot collide. */
    std::uint32_t key = c.rt->next_tls_key++;
    if (c.arg(0) != 0) c.write32(c.arg(0), key);
    c.set_result(0);
}

void c_key_delete(GuestCall &c) {
    guest_tls::values().erase(c.arg(0));
    c.set_result(0);
}

void c_setspecific(GuestCall &c) {
    guest_tls::values()[c.arg(0)] = c.arg(1);
    c.set_result(0);
}

void c_getspecific(GuestCall &c) {
    auto &values = guest_tls::values();
    auto found = values.find(c.arg(0));
    c.set_result(found == values.end() ? 0u : found->second);
}

/* ------------------------------------------------------ attrs and sched
 *
 * Every guest thread gets a fixed 1MB stack from kThreadStackBase and the
 * host's default scheduling, so the attribute setters have nothing to record.
 * They still must return 0: a non-zero result makes bionic's own wrappers treat
 * the whole pthread_create as failed. */
void c_ok(GuestCall &c) { c.set_result(0); }

void c_attr_getstack(GuestCall &c) {
    /* (attr, &stackaddr, &stacksize). The engine uses this to sanity-check how
     * much room it has; answer with the real geometry from guest_runtime.h. */
    if (c.arg(1) != 0) c.write32(c.arg(1), 0x02000000);
    if (c.arg(2) != 0) c.write32(c.arg(2), 0x00100000);
    c.set_result(0);
}

void c_sched_get_priority(GuestCall &c) { c.set_result(0); } /* one priority level: the host's */
void c_sched_yield(GuestCall &c) {
    std::this_thread::yield();
    c.set_result(0);
}

/* ------------------------------------------------------------- rwlocks
 *
 * New in 9.6.1. Backed by a real GuestRwLock rather than reusing GuestMutex,
 * because a recursive READ lock is legal and would deadlock against a mutex --
 * see the type's comment in guest_sync.h. */
void c_rwlock_init(GuestCall &c) {
    c.rt->get_or_create_rwlock(c.arg(0));
    c.set_result(0);
}

void c_rwlock_destroy(GuestCall &c) {
    /* The object stays in the map. Destroying it would race any thread still
     * inside a wait on it, and the map is small and bounded by the number of
     * distinct guest addresses. */
    c.set_result(0);
}

void c_rwlock_rdlock(GuestCall &c) {
    c.note_blocked(); /* progress, not a spin -- keeps a worker off the runaway cap */
    c.rt->get_or_create_rwlock(c.arg(0))->rdlock();
    c.set_result(0);
}

void c_rwlock_wrlock(GuestCall &c) {
    c.note_blocked();
    c.rt->get_or_create_rwlock(c.arg(0))->wrlock(guest_tls::self_id);
    c.set_result(0);
}

void c_rwlock_unlock(GuestCall &c) {
    c.rt->get_or_create_rwlock(c.arg(0))->unlock();
    c.set_result(0);
}

/* int pthread_atfork(void (*prepare)(), void (*parent)(), void (*child)())
 *
 * Records nothing and succeeds. This port's fork() always fails (see
 * libc_socket.cpp's reasoning -- the callers are Crashlytics and analytics), so
 * a handler registered here could never run. Returning 0 is what the caller
 * needs to continue; failing would abort a library's initialiser over a
 * hypothetical. */
void c_atfork(GuestCall &c) { c.set_result(0); }

}  // namespace

void register_libc_pthread(ImportTable &t) {
    t.add("pthread_mutex_init", c_mutex_init);
    t.add("pthread_mutex_destroy", c_mutex_destroy);
    t.add("pthread_mutex_lock", c_mutex_lock);
    t.add("pthread_mutex_unlock", c_mutex_unlock);
    t.add("pthread_mutex_trylock", c_mutex_trylock);

    t.add("pthread_cond_init", c_cond_init);
    t.add("pthread_cond_destroy", c_cond_destroy);
    t.add("pthread_cond_signal", c_cond_signal);
    t.add("pthread_cond_broadcast", c_cond_broadcast);
    t.add("pthread_cond_wait", c_cond_wait);
    t.add("pthread_cond_timedwait", c_cond_timedwait);

    t.add("sem_init", c_sem_init);
    t.add("sem_destroy", c_sem_destroy);
    t.add("sem_wait", c_sem_wait);
    t.add("sem_trywait", c_sem_trywait);
    t.add("sem_timedwait", c_sem_timedwait);
    t.add("sem_post", c_sem_post);
    t.add("sem_getvalue", c_sem_getvalue);

    t.add("pthread_once", c_once);
    t.add("pthread_create", c_create);
    t.add("pthread_join", c_join);
    t.add("pthread_detach", c_detach);
    t.add("pthread_self", c_self);
    t.add("pthread_equal", c_equal);
    t.add("pthread_exit", c_exit);

    t.add("pthread_key_create", c_key_create);
    t.add("pthread_key_delete", c_key_delete);
    t.add("pthread_setspecific", c_setspecific);
    t.add("pthread_getspecific", c_getspecific);

    /* Added for 9.6.1. */
    t.add("pthread_rwlock_init", c_rwlock_init);
    t.add("pthread_rwlock_destroy", c_rwlock_destroy);
    t.add("pthread_rwlock_rdlock", c_rwlock_rdlock);
    t.add("pthread_rwlock_wrlock", c_rwlock_wrlock);
    t.add("pthread_rwlock_unlock", c_rwlock_unlock);
    t.add("pthread_atfork", c_atfork);

    /* Attributes: accepted and ignored -- see c_ok. */
    t.add("pthread_attr_init", c_ok);
    t.add("pthread_attr_destroy", c_ok);
    t.add("pthread_attr_setdetachstate", c_ok);
    t.add("pthread_attr_setschedparam", c_ok);
    t.add("pthread_attr_setschedpolicy", c_ok);
    t.add("pthread_attr_setstack", c_ok);
    t.add("pthread_attr_setstacksize", c_ok);
    t.add("pthread_attr_getschedparam", c_ok);
    t.add("pthread_attr_getstack", c_attr_getstack);
    t.add("pthread_getattr_np", c_ok);
    t.add("pthread_getschedparam", c_ok);
    t.add("pthread_setschedparam", c_ok);
    t.add("pthread_mutexattr_init", c_mutexattr_init);
    t.add("pthread_mutexattr_destroy", c_ok);
    t.add("pthread_mutexattr_settype", c_mutexattr_settype);
    t.add("pthread_mutexattr_gettype", c_mutexattr_gettype);
    t.add("pthread_mutexattr_setpshared", c_ok);
    t.add("pthread_condattr_init", c_ok);
    t.add("pthread_condattr_destroy", c_ok);

    t.add("sched_get_priority_min", c_sched_get_priority);
    t.add("sched_get_priority_max", c_sched_get_priority);
    t.add("sched_yield", c_sched_yield);
}

}  // namespace pvz2native
