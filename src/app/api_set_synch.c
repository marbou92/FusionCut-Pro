// FusionCut Pro - api-ms-win-core-synch-l1-2-0.dll compatibility shim.
//
// Built by CMake as the target "fcp-apiset-synch" and installed next to
// FusionCutPro.exe with its EXACT api-set name (see src/app/CMakeLists.txt:
// OUTPUT_NAME "api-ms-win-core-synch-l1-2-0", PREFIX ""). It replaces the
// v0.4.6 packaging/api-ms-win-core-synch-l1-2-0.def forwarder stub, which
// never shipped (its CI build step died on a bash quoting bug) and which
// could not have worked on Windows 7 anyway - see WHY REAL IMPLEMENTATIONS
// below.
//
// WHY THIS DLL EXISTS
//   librav1e.dll (the Rust-written AV1 encoder in MSYS2's FFmpeg 8
//   dependency tree) hard-imports 'api-ms-win-core-synch-l1-2-0.dll' -
//   the WaitOnAddress futex API set - by its literal api-set name.
//   Diagnosed live by fcp-loader-check.exe v2.1's debug-launch watch
//   (v0.4.5 field data): first-chance 0xC0000005 READ at ntdll+0x4b4b4
//   (the loader's import-snapping code), targeting an address inside
//   the just-mapped api-ms-win-core-synch-l1-2-0.dll image (base +
//   0x13f3), load trail stopped at FusionCutPro.exe -> avcodec-62.dll
//   -> ... -> librav1e.dll -> api-ms-win-core-synch-l1-2-0.dll.
//
//   On Windows 8.x the loader maps the System32 placeholder stub for
//   that name and faults reading its invalid export data while
//   snapping librav1e's imports - before any user code runs, so WER
//   never engages (no Event Viewer entry, no crash log; only the bare
//   "unable to start correctly (0xc0000005)" dialog).
//
//   On Windows 7 the name resolves to no file at all (Win7 has neither
//   the schema entry nor a System32 placeholder for it).
//
//   The DLL search order tries the application directory BEFORE
//   System32, so a real DLL with this exact name placed next to
//   FusionCutPro.exe wins the bind on Windows 7 and 8.x. On
//   Windows 10/11 the ApiSetSchema resolves the name to KernelBase
//   before the file search runs, so this DLL is never even touched -
//   shipping it everywhere is safe and inert.
//
// WHY REAL IMPLEMENTATIONS (not forwarders, as the v0.4.6 attempt)
//   A forwarder-only stub would export WaitOnAddress as a forwarder
//   into KernelBase - but KernelBase only implements the futex family
//   since Windows 8. On Windows 7 the import would bind against OUR
//   export table, then entry-point resolution would follow the
//   forwarder, fail to find KernelBase!WaitOnAddress, and abort the
//   load of librav1e.dll with "The procedure entry point WaitOnAddress
//   could not be located" - trading one startup failure for another.
//   The user's test platform is Windows 7, so this shim carries REAL
//   implementations built exclusively from Windows-7-era kernel32
//   primitives:
//     * SRWLOCK and CONDITION_VARIABLE (both Vista+, and both are
//       validly initialized by all-zero memory - the whole shim's
//       state lives in zero-initialized .bss, so DllMain has no
//       lock initialization to do at all),
//     * SleepConditionVariableSRW / WakeConditionVariable /
//       WakeAllConditionVariable (Vista+) - note the Windows
//       condition-variable wake surface is EXACTLY those two
//       functions; there is no SignalConditionVariable in the Win32
//       API (v0.4.7 named that phantom and MinGW CI aborted on the
//       implicit-declaration error),
//     * GetTickCount64 (Vista+) for timeout bookkeeping.
//
// SEMANTICS IMPLEMENTED (documented contract of the api set)
//   WaitOnAddress(Address, CompareAddress, AddressSize, dwMilliseconds)
//     Blocks while the value at Address still equals the value at
//     CompareAddress. Returns TRUE once the value differs; returns
//     FALSE with GetLastError() == ERROR_TIMEOUT when dwMilliseconds
//     elapses first. AddressSize must be 1, 2, 4 or 8.
//   WakeByAddressAll(Address)
//     Wakes every thread currently blocked in WaitOnAddress on Address.
//   WakeByAddressSingle(Address)
//     Wakes one such thread.
//   Sleep(dwMilliseconds)
//     Suspends the calling thread (present in the real api set;
//     implemented as a timed wait on a private, never-signaled
//     condition variable so the DLL needs no kernel32 Sleep import
//     that could collide with this export's name).
//
// CONCURRENCY DESIGN (the lost-wake problem, solved)
//   Wait state lives in a fixed hash table of 512 buckets keyed by the
//   waited address; each bucket is an SRWLOCK, each active address in a
//   bucket has one wait node with a CONDITION_VARIABLE and a waiter
//   count. Nodes come from a static pool of 4096 (threaded onto a free
//   list once in DllMain - see below).
//
//   The waiter holds the bucket lock across BOTH the value comparison
//   AND the SleepConditionVariableSRW call. That is the atomicity the
//   real WaitOnAddress provides: the waker's WakeByAddress* must take
//   the same bucket lock before it can wake anyone, and
//   SleepConditionVariableSRW only releases the lock once the thread is
//   registered on the condition variable - so a wake that happens
//   between "value compared equal" and "thread asleep" is impossible
//   to miss. After waking, the waiter re-checks the value under the
//   lock and re-sleeps if it is still equal (spurious wakeups are part
//   of the contract; callers loop on the value themselves).
//
//   A node returns to the pool when its waiter count drops to zero
//   (under the bucket lock). Lock order is always bucket -> free-list;
//   wakers take only the bucket lock, so no path can deadlock.
//
// CRT-FREE BUILD CONSTRAINTS
//   The DLL links with -nostdlib and imports kernel32 ONLY. That
//   imposes code rules, all followed here:
//     * no CRT function calls (no memset/memcpy/printf);
//     * no aggregate operations that would make GCC emit calls to
//       memset/memcpy - every struct field is assigned individually
//       and all global state is zero-initialized .bss (the loader
//       zeroes it; zero IS the initialized state for SRWLOCK and
//       CONDITION_VARIABLE on x64);
//     * no 64-bit division (the bucket hash uses shifts and a mask,
//       so libgcc helpers like __udivdi3 are never needed on any
//       architecture GCC might target this with);
//     * no TLS, no exceptions, no static constructors - DllMain does
//       pointer writes only (legal under the loader lock).
//
//   DllMain threads the static node pool onto the free list at
//   PROCESS_ATTACH. The loader calls DllMain before any export of this
//   DLL can execute, and the only tool that maps the DLL WITHOUT
//   running DllMain is fcp-loader-check.exe phase 1
//   (DONT_RESOLVE_DLL_REFERENCES), which walks export tables and never
//   calls these functions - so the free list is always valid when it
//   is used. If the pool is nevertheless exhausted at runtime,
//   WaitOnAddress fails loudly with ERROR_NOT_ENOUGH_MEMORY instead of
//   corrupting state (4096 distinct concurrently-waited addresses is
//   far beyond anything rav1e's thread pool does).

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

#define FCP_BUCKET_COUNT 512u
#define FCP_NODE_POOL 4096u

typedef struct FcpWaitNode {
    struct FcpWaitNode *hash_next; // chain inside its bucket
    struct FcpWaitNode *free_next; // chain on the free list
    CONDITION_VARIABLE cv;         // zeroed == initialized (x64 MinGW)
    LONG waiters;                  // guarded by the bucket lock
    const void *address;           // the futex key; NULL == free node
} FcpWaitNode;

typedef struct {
    SRWLOCK lock; // zeroed == initialized (x64 MinGW)
    FcpWaitNode *head;
} FcpBucket;

static FcpBucket g_buckets[FCP_BUCKET_COUNT];
static FcpWaitNode g_pool[FCP_NODE_POOL];
static FcpWaitNode *g_free_head;
static SRWLOCK g_free_lock;

// Forward declarations of the exported api-set surface. windows.h
// (synchapi.h) declared Sleep with __declspec(dllimport) (its real home
// is kernel32); re-declaring it with dllexport - THIS DLL is the api
// set - makes GCC merge the declarations and warn "redeclared without
// dllimport attribute". The merged symbol is dllexport, which is
// exactly the state we want, so the warning is silenced for this block
// only; no other diagnostic is affected.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
__declspec(dllexport) BOOL WINAPI WaitOnAddress(volatile VOID *Address, PVOID CompareAddress,
                                                SIZE_T AddressSize, DWORD dwMilliseconds);
__declspec(dllexport) VOID WINAPI WakeByAddressAll(PVOID Address);
__declspec(dllexport) VOID WINAPI WakeByAddressSingle(PVOID Address);
__declspec(dllexport) VOID WINAPI Sleep(DWORD dwMilliseconds);
#pragma GCC diagnostic pop

// Bucket index for a futex address: shift + fold + mask. Adjacent
// stack/heap words land in different buckets; no division anywhere.
// Takes the volatile-qualified pointer the api hands us (WaitOnAddress
// receives volatile VOID *) so no qualifier is silently discarded.
static SIZE_T fcp_bucket_index(const volatile void *address) {
    ULONG64 key = (ULONG64)(uintptr_t)address;
    key ^= key >> 17;
    key ^= key >> 31;
    return (SIZE_T)(key & (FCP_BUCKET_COUNT - 1u));
}

// TRUE when the value at Address no longer equals the value at
// CompareAddress. The waiter calls this with the bucket lock held, and
// the waker's value store happens before it takes the same lock, so the
// re-check always observes the change that prompted the wake (x86
// total store order + the SRWLock barriers make the pairing sound).
static BOOL fcp_value_differs(const volatile void *address, const void *compare, SIZE_T size) {
    switch (size) {
    case 1:
        return *(const volatile UCHAR *)address != *(const UCHAR *)compare;
    case 2:
        return *(const volatile USHORT *)address != *(const USHORT *)compare;
    case 4:
        return *(const volatile ULONG *)address != *(const ULONG *)compare;
    default:
        return *(const volatile ULONG64 *)address != *(const ULONG64 *)compare;
    }
}

// Find the wait node for Address in its bucket, or take one from the
// free pool and link it in. Bucket lock must be held exclusively.
// Returns NULL only when the static pool is exhausted.
static FcpWaitNode *fcp_node_acquire(FcpBucket *bucket, const void *address) {
    FcpWaitNode *node;
    for (node = bucket->head; node != NULL; node = node->hash_next) {
        if (node->address == address)
            return node;
    }
    AcquireSRWLockExclusive(&g_free_lock);
    node = g_free_head;
    if (node != NULL)
        g_free_head = node->free_next;
    ReleaseSRWLockExclusive(&g_free_lock);
    if (node == NULL)
        return NULL;
    node->cv.Ptr = NULL; // recycled node: re-zero the condition variable
    node->waiters = 0;
    node->free_next = NULL;
    node->address = address;
    node->hash_next = bucket->head;
    bucket->head = node;
    return node;
}

// Return a zero-waiter node to the free pool. Bucket lock must be held
// exclusively; the caller must have verified waiters == 0.
static void fcp_node_release(FcpBucket *bucket, FcpWaitNode *node) {
    FcpWaitNode **link = &bucket->head;
    while (*link != NULL && *link != node)
        link = &(*link)->hash_next;
    if (*link == node)
        *link = node->hash_next;
    node->hash_next = NULL;
    node->address = NULL;
    AcquireSRWLockExclusive(&g_free_lock);
    node->free_next = g_free_head;
    g_free_head = node;
    ReleaseSRWLockExclusive(&g_free_lock);
}

__declspec(dllexport) BOOL WINAPI WaitOnAddress(volatile VOID *Address, PVOID CompareAddress,
                                                SIZE_T AddressSize, DWORD dwMilliseconds) {
    FcpBucket *bucket;
    FcpWaitNode *node;
    ULONGLONG deadline;
    BOOL changed = FALSE;

    switch (AddressSize) {
    case 1:
    case 2:
    case 4:
    case 8:
        break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    bucket = &g_buckets[fcp_bucket_index(Address)];
    AcquireSRWLockExclusive(&bucket->lock);
    node = fcp_node_acquire(bucket, (const void *)Address);
    if (node == NULL) {
        ReleaseSRWLockExclusive(&bucket->lock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    deadline = (dwMilliseconds == INFINITE) ? 0 : GetTickCount64() + dwMilliseconds;
    for (;;) {
        DWORD wait_ms;
        BOOL ok;

        if (fcp_value_differs((const volatile void *)Address, CompareAddress, AddressSize)) {
            changed = TRUE;
            break;
        }
        if (dwMilliseconds == INFINITE) {
            wait_ms = INFINITE;
        } else {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                break; // timed out
            wait_ms = (DWORD)(deadline - now);
        }

        // Holding the bucket lock across compare-and-sleep is what makes
        // a concurrent WakeByAddress* impossible to miss: it cannot take
        // this lock (and thus cannot wake) until this thread is already
        // queued on the condition variable and the lock has been dropped
        // inside the call below.
        node->waiters++;
        ok = SleepConditionVariableSRW(&node->cv, &bucket->lock, wait_ms, 0);
        node->waiters--;

        if (fcp_value_differs((const volatile void *)Address, CompareAddress, AddressSize)) {
            // Covers both a genuine wake and a wake racing a timeout: the
            // value changed, so report success either way.
            changed = TRUE;
            break;
        }
        if (!ok) {
            // Timeout (or an exotic failure - reported as timeout so
            // callers that only understand ERROR_TIMEOUT stay robust).
            break;
        }
    }

    if (node->waiters == 0)
        fcp_node_release(bucket, node);
    ReleaseSRWLockExclusive(&bucket->lock);

    if (changed)
        return TRUE;
    SetLastError(ERROR_TIMEOUT);
    return FALSE;
}

static void fcp_wake(PVOID Address, BOOL wake_all) {
    FcpBucket *bucket = &g_buckets[fcp_bucket_index(Address)];
    FcpWaitNode *node;

    AcquireSRWLockExclusive(&bucket->lock);
    for (node = bucket->head; node != NULL; node = node->hash_next) {
        if (node->address == (const void *)Address) {
            // The Vista+ condition-variable wake surface is exactly two
            // functions: WakeAllConditionVariable (broadcast, every
            // waiter) and WakeConditionVariable (exactly ONE waiter).
            // v0.4.7 had this backwards - WakeByAddressAll called the
            // wake-one function, and the single-wake branch named
            // SignalConditionVariable, a function that does not exist
            // in the Win32 API at all (MinGW CI: implicit-declaration
            // error). Correct mapping per the documented api-set
            // contract: WakeByAddressAll -> broadcast, WakeByAddress
            // Single -> wake one.
            if (wake_all)
                WakeAllConditionVariable(&node->cv);
            else
                WakeConditionVariable(&node->cv);
            break;
        }
    }
    ReleaseSRWLockExclusive(&bucket->lock);
}

__declspec(dllexport) VOID WINAPI WakeByAddressAll(PVOID Address) {
    fcp_wake(Address, TRUE);
}

__declspec(dllexport) VOID WINAPI WakeByAddressSingle(PVOID Address) {
    fcp_wake(Address, FALSE);
}

__declspec(dllexport) VOID WINAPI Sleep(DWORD dwMilliseconds) {
    // A private, never-signaled condition variable plus an exclusive
    // lock nobody else knows about: the timed wait below releases the
    // lock, blocks for dwMilliseconds (nothing can ever wake it), then
    // reacquires it. That is a faithful Sleep without importing
    // kernel32!Sleep, whose name this export already occupies.
    CONDITION_VARIABLE cv;
    SRWLOCK lock;

    cv.Ptr = NULL;
    lock.Ptr = NULL;
    AcquireSRWLockExclusive(&lock);
    SleepConditionVariableSRW(&cv, &lock, dwMilliseconds, 0);
    ReleaseSRWLockExclusive(&lock);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DWORD i;
        // Pure pointer writes - legal under the loader lock. Threading
        // the free list here (instead of lazily) keeps node allocation
        // lock-ordered and race-free; see the header comment.
        DisableThreadLibraryCalls(instance);
        for (i = 0; i + 1 < FCP_NODE_POOL; i++)
            g_pool[i].free_next = &g_pool[i + 1];
        g_pool[FCP_NODE_POOL - 1u].free_next = NULL;
        g_free_head = &g_pool[0];
    }
    return TRUE;
}
