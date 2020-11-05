/*
 * Copyright 2010-2017 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Alloc.h"
#include "Atomic.h"
#include "Cleaner.h"
#include "Exceptions.h"
#include "KAssert.h"
#include "Memory.h"
#include "ObjCExportInit.h"
#include "Porting.h"
#include "Runtime.h"
#include "Worker.h"

typedef void (*Initializer)(int initialize, MemoryState* memory);
struct InitNode {
  Initializer init;
  InitNode* next;
};

namespace {

InitNode* initHeadNode = nullptr;
InitNode* initTailNode = nullptr;

enum class RuntimeStatus {
    kUninitialized,
    kRunning,
    kDestroying,
};

struct RuntimeState {
    MemoryState* memoryState;
    Worker* worker;
    RuntimeStatus status = RuntimeStatus::kUninitialized;
};

enum {
  INIT_GLOBALS = 0,
  INIT_THREAD_LOCAL_GLOBALS = 1,
  DEINIT_THREAD_LOCAL_GLOBALS = 2,
  DEINIT_GLOBALS = 3
};

void InitOrDeinitGlobalVariables(int initialize, MemoryState* memory) {
  InitNode* currentNode = initHeadNode;
  while (currentNode != nullptr) {
    currentNode->init(initialize, memory);
    currentNode = currentNode->next;
  }
}

KBoolean g_checkLeaks = KonanNeedDebugInfo;
KBoolean g_checkLeakedCleaners = KonanNeedDebugInfo;

constexpr RuntimeState* kInvalidRuntime = nullptr;

THREAD_LOCAL_VARIABLE RuntimeState* runtimeState = kInvalidRuntime;

inline bool isValidRuntime() {
  return ::runtimeState != kInvalidRuntime;
}

volatile int aliveRuntimesCount = 0;

enum GlobalRuntimeStatus {
    kGlobalRuntimeUninitialized = 0,
    kGlobalRuntimeRunning,
    kGlobalRuntimeShuttingDown,
    kGlobalRuntimeShutdown,
};

volatile GlobalRuntimeStatus globalRuntimeStatus = kGlobalRuntimeUninitialized;

class ScopedInitializingRuntime {
 public:
  ScopedInitializingRuntime() {
    atomicAdd(&initializingRuntimesCount, 1);
  }

  ~ScopedInitializingRuntime() {
    atomicAdd(&initializingRuntimesCount, -1);
  }

  static bool IsInitializing() {
    return atomicGet(&initializingRuntimesCount) > 0;
  }

  ScopedInitializingRuntime(const ScopedInitializingRuntime&) = delete;
  ScopedInitializingRuntime(ScopedInitializingRuntime&&) = delete;
  ScopedInitializingRuntime& operator=(const ScopedInitializingRuntime&) = delete;
  ScopedInitializingRuntime& operator=(ScopedInitializingRuntime&&) = delete;

  private:
    static int initializingRuntimesCount;
};

// static
int ScopedInitializingRuntime::initializingRuntimesCount = 0;

RuntimeState* initRuntime() {
  ScopedInitializingRuntime guard;

  auto lastStatus = compareAndSwap(&globalRuntimeStatus, kGlobalRuntimeUninitialized, kGlobalRuntimeRunning);
  if (lastStatus == kGlobalRuntimeShutdown) {
      konan::consoleErrorf("Kotlin runtime was shut down. Cannot create new runtimes\n");
      konan::abort();
  }

  SetKonanTerminateHandler();
  RuntimeState* result = konanConstructInstance<RuntimeState>();
  if (!result) return kInvalidRuntime;
  RuntimeCheck(!isValidRuntime(), "No active runtimes allowed");
  ::runtimeState = result;
  result->memoryState = InitMemory();
  result->worker = WorkerInit(true);
  bool firstRuntime = atomicAdd(&aliveRuntimesCount, 1) == 1;
  // Keep global variables in state as well.
  if (firstRuntime) {
    konan::consoleInit();
#if KONAN_OBJC_INTEROP
    Kotlin_ObjCExport_initialize();
#endif
    InitOrDeinitGlobalVariables(INIT_GLOBALS, result->memoryState);
  }
  InitOrDeinitGlobalVariables(INIT_THREAD_LOCAL_GLOBALS, result->memoryState);
  RuntimeAssert(result->status == RuntimeStatus::kUninitialized, "Runtime must still be in the uninitialized state");
  result->status = RuntimeStatus::kRunning;
  return result;
}

void deinitRuntime(RuntimeState* state) {
  RuntimeAssert(state->status == RuntimeStatus::kRunning, "Runtime must be in the running state");
  state->status = RuntimeStatus::kDestroying;
  // This may be called after TLS is zeroed out, so ::memoryState in Memory cannot be trusted.
  RestoreMemory(state->memoryState);
  bool lastRuntime = atomicAdd(&aliveRuntimesCount, -1) == 0;
  InitOrDeinitGlobalVariables(DEINIT_THREAD_LOCAL_GLOBALS, state->memoryState);
  if (lastRuntime)
    InitOrDeinitGlobalVariables(DEINIT_GLOBALS, state->memoryState);
  auto workerId = GetWorkerId(state->worker);
  WorkerDeinit(state->worker);
  DeinitMemory(state->memoryState);
  konanDestructInstance(state);
  WorkerDestroyThreadDataIfNeeded(workerId);
}

void Kotlin_deinitRuntimeCallback(void* argument) {
  auto* state = reinterpret_cast<RuntimeState*>(argument);
  deinitRuntime(state);
}

}  // namespace

extern "C" {

void AppendToInitializersTail(InitNode *next) {
  // TODO: use RuntimeState.
  if (initHeadNode == nullptr) {
    initHeadNode = next;
  } else {
    initTailNode->next = next;
  }
  initTailNode = next;
}

void Kotlin_initRuntimeIfNeeded() {
  if (!isValidRuntime()) {
    initRuntime();
    // Register runtime deinit function at thread cleanup.
    konan::onThreadExit(Kotlin_deinitRuntimeCallback, runtimeState);
  }
}

void Kotlin_deinitRuntimeIfNeeded() {
  if (isValidRuntime()) {
    deinitRuntime(::runtimeState);
    ::runtimeState = kInvalidRuntime;
  }
}

// TODO: Consider exporting it to interop API.
void Kotlin_shutdownRuntime() {
    // TODO: If checkers are disabled, we can set status to "shutdown" here, and return.
    auto lastStatus = compareAndSwap(&globalRuntimeStatus, kGlobalRuntimeRunning, kGlobalRuntimeShuttingDown);
    switch (lastStatus) {
        case kGlobalRuntimeRunning:
            break;
        case kGlobalRuntimeShuttingDown:
        case kGlobalRuntimeShutdown:
            konan::consoleErrorf("Cannot shutdown Kotlin runtime twice\n");
            konan::abort();
        case kGlobalRuntimeUninitialized:
            konan::consoleErrorf("Kotlin runtime must have been initialized\n");
            konan::abort();
    }

    auto* runtime = ::runtimeState;
    if (runtime == kInvalidRuntime) {
        konan::consoleErrorf("Current thread must have Kotlin runtime initialized on it\n");
        konan::abort();
    }

    if (Kotlin_cleanersLeakCheckerEnabled()) {
        // Make sure to collect any lingering cleaners.
        PerformFullGC(runtime->memoryState);
    }

    // Stop cleaner worker. Only execute the cleaners if checker is enabled.
    ShutdownCleaners(Kotlin_cleanersLeakCheckerEnabled());

    // Cleaners are now done, disallow new runtimes.
    lastStatus = compareAndSwap(&globalRuntimeStatus, kGlobalRuntimeShuttingDown, kGlobalRuntimeShutdown);
    RuntimeAssert(lastStatus == kGlobalRuntimeShuttingDown, "Must be in ShuttingDown state");

    // Spin until all runtimes have fully initialized.
    while (ScopedInitializingRuntime::IsInitializing()) {}

    // TODO: If we add early return at the top, this if would be unneeded.
    if (Kotlin_memoryLeakCheckerEnabled() || Kotlin_memoryLeakCheckerEnabled()) {
        // First make sure workers are gone.
        WaitNativeWorkersTermination();

        // Now check for existence of any other runtimes.
        // `aliveRuntimesCount` can only go down, because we forbade new runtimes initialization.
        auto otherRuntimesCount = atomicGet(&aliveRuntimesCount) - 1;
        RuntimeAssert(otherRuntimesCount >= 0, "Cannot be negative");
        if (otherRuntimesCount > 0) {
            konan::consoleErrorf("Cannot run checkers when there are %d alive runtimes at the shutdown", otherRuntimesCount);
            konan::abort();
        }
    }

    deinitRuntime(runtime);
    ::runtimeState = kInvalidRuntime;
}

KInt Konan_Platform_canAccessUnaligned() {
#if KONAN_NO_UNALIGNED_ACCESS
  return 0;
#else
  return 1;
#endif
}

KInt Konan_Platform_isLittleEndian() {
#ifdef __BIG_ENDIAN__
  return 0;
#else
  return 1;
#endif
}

KInt Konan_Platform_getOsFamily() {
#if KONAN_MACOSX
  return 1;
#elif KONAN_IOS
  return 2;
#elif KONAN_LINUX
  return 3;
#elif KONAN_WINDOWS
  return 4;
#elif KONAN_ANDROID
  return 5;
#elif KONAN_WASM
  return 6;
#elif KONAN_TVOS
  return 7;
#elif KONAN_WATCHOS
  return 8;
#else
#warning "Unknown platform"
  return 0;
#endif
}

KInt Konan_Platform_getCpuArchitecture() {
#if KONAN_ARM32
  return 1;
#elif KONAN_ARM64
  return 2;
#elif KONAN_X86
  return 3;
#elif KONAN_X64
  return 4;
#elif KONAN_MIPS32
  return 5;
#elif KONAN_MIPSEL32
  return 6;
#elif KONAN_WASM
  return 7;
#else
#warning "Unknown CPU"
  return 0;
#endif
}

KInt Konan_Platform_getMemoryModel() {
  return IsStrictMemoryModel ? 0 : 1;
}

KBoolean Konan_Platform_isDebugBinary() {
  return KonanNeedDebugInfo ? true : false;
}

bool Kotlin_memoryLeakCheckerEnabled() {
  return g_checkLeaks;
}

KBoolean Konan_Platform_getMemoryLeakChecker() {
  return g_checkLeaks;
}

void Konan_Platform_setMemoryLeakChecker(KBoolean value) {
  g_checkLeaks = value;
}

bool Kotlin_cleanersLeakCheckerEnabled() {
    return g_checkLeakedCleaners;
}

KBoolean Konan_Platform_getCleanersLeakChecker() {
    return g_checkLeakedCleaners;
}

void Konan_Platform_setCleanersLeakChecker(KBoolean value) {
    g_checkLeakedCleaners = value;
}

}  // extern "C"
