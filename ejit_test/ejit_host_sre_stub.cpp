// Host-only SRE platform stubs for ejit_test.
//
// These definitions let Linux integration-test binaries link against an
// EJIT_FREESTANDING + SRE code-pool/taskpool runtime. They are not intended for
// target/SRE deployment, where the real platform supplies these symbols.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using TSK_ARG_T = uint64_t;
using TSK_ENTRY_FUNC = void (*)(TSK_ARG_T, TSK_ARG_T, TSK_ARG_T, TSK_ARG_T);

struct TSK_INIT_PARAM_S {
  TSK_ENTRY_FUNC pfnTaskEntry;
  uint16_t usTaskPrio;
  uint16_t usResved;
  TSK_ARG_T auwArgs[4];
  uint32_t uwStackSize;
  const char *pcName;
  uint32_t uwResved;
  TSK_ARG_T uwPrivateData;
};

struct HostTaskArgs {
  TSK_ENTRY_FUNC entry;
  TSK_ARG_T args[4];
};

constexpr uint32_t kMaxHostTasks = 1024;

pthread_mutex_t gTaskMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t gTasks[kMaxHostTasks];
bool gTaskLive[kMaxHostTasks];
uint32_t gNextTaskId = 1;

void *hostTaskMain(void *Opaque) {
  HostTaskArgs *Args = static_cast<HostTaskArgs *>(Opaque);
  TSK_ENTRY_FUNC Entry = Args->entry;
  TSK_ARG_T A0 = Args->args[0];
  TSK_ARG_T A1 = Args->args[1];
  TSK_ARG_T A2 = Args->args[2];
  TSK_ARG_T A3 = Args->args[3];
  delete Args;
  if (Entry)
    Entry(A0, A1, A2, A3);
  return nullptr;
}

uintptr_t alignDown(uintptr_t V, uintptr_t Align) { return V & ~(Align - 1); }

} // namespace

extern "C" uint32_t ejit_sre_current_core_id() { return 0; }

extern "C" void *SRE_MemDbgAlloc(unsigned int, unsigned char,
                                 unsigned long Size, const char *,
                                 unsigned int) {
  void *Ptr = nullptr;
  constexpr size_t Align2M = 2u * 1024u * 1024u;
  if (posix_memalign(&Ptr, Align2M, static_cast<size_t>(Size)) != 0)
    return nullptr;
  std::memset(Ptr, 0, static_cast<size_t>(Size));
  return Ptr;
}

extern "C" unsigned split_2m_to_4k(unsigned long long, unsigned long long) {
  return 0;
}

extern "C" unsigned enable_ex(unsigned, unsigned long long Va) {
  long Page = sysconf(_SC_PAGESIZE);
  if (Page <= 0)
    Page = 4096;
  uintptr_t PageVA = alignDown(static_cast<uintptr_t>(Va),
                               static_cast<uintptr_t>(Page));
  __builtin___clear_cache(reinterpret_cast<char *>(PageVA),
                          reinterpret_cast<char *>(PageVA + Page));
  if (mprotect(reinterpret_cast<void *>(PageVA), static_cast<size_t>(Page),
               PROT_READ | PROT_EXEC) != 0)
    return 1;
  return 0;
}

extern "C" uint32_t SRE_TaskCreate(uint32_t *TaskPid,
                                   TSK_INIT_PARAM_S *InitParam) {
  if (!TaskPid || !InitParam || !InitParam->pfnTaskEntry)
    return 1;

  HostTaskArgs *Args = new HostTaskArgs;
  Args->entry = InitParam->pfnTaskEntry;
  for (unsigned I = 0; I < 4; ++I)
    Args->args[I] = InitParam->auwArgs[I];

  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  if (InitParam->uwStackSize)
    pthread_attr_setstacksize(&Attr, InitParam->uwStackSize);

  pthread_t Thread{};
  int Rc = pthread_create(&Thread, &Attr, hostTaskMain, Args);
  pthread_attr_destroy(&Attr);
  if (Rc != 0) {
    delete Args;
    return static_cast<uint32_t>(Rc);
  }

  pthread_mutex_lock(&gTaskMutex);
  uint32_t Id = gNextTaskId++;
  if (Id >= kMaxHostTasks)
    Id = 1;
  while (gTaskLive[Id]) {
    Id = (Id + 1) % kMaxHostTasks;
    if (Id == 0)
      Id = 1;
  }
  gTasks[Id] = Thread;
  gTaskLive[Id] = true;
  *TaskPid = Id;
  pthread_mutex_unlock(&gTaskMutex);
  return 0;
}

extern "C" uint32_t SRE_TaskDelete(uint32_t TaskPid) {
  if (TaskPid == 0 || TaskPid >= kMaxHostTasks)
    return 1;

  pthread_t Thread{};
  pthread_mutex_lock(&gTaskMutex);
  if (!gTaskLive[TaskPid]) {
    pthread_mutex_unlock(&gTaskMutex);
    return 1;
  }
  Thread = gTasks[TaskPid];
  gTaskLive[TaskPid] = false;
  pthread_mutex_unlock(&gTaskMutex);

  return static_cast<uint32_t>(pthread_join(Thread, nullptr));
}

extern "C" uint32_t SRE_TaskDelay(uint32_t Tick) {
  if (Tick == 0) {
    sched_yield();
    return 0;
  }
  usleep(static_cast<useconds_t>(Tick) * 1000u);
  return 0;
}
