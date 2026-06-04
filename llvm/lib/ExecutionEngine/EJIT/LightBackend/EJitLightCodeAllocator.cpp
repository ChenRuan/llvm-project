//===- EJitLightCodeAllocator.cpp - light backend code allocators ---------===//
//
// HostMmapCodeAllocator 与 StaticSlabCodeAllocator 的实现。详见头文件。
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightCodeAllocator.h"

#include "llvm/Support/Errc.h"

#include <cstring>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace llvm;
using namespace llvm::ejit::light;

//===----------------------------------------------------------------------===//
// HostMmapCodeAllocator
//===----------------------------------------------------------------------===//

HostMmapCodeAllocator::~HostMmapCodeAllocator() {
#if !defined(_WIN32)
  for (auto &R : Regions)
    if (R.Base)
      ::munmap(R.Base, R.MapLen);
#endif
  Regions.clear();
}

Expected<MutableArrayRef<uint8_t>>
HostMmapCodeAllocator::allocate(std::size_t Size, std::size_t Align) {
#if defined(_WIN32)
  (void)Size;
  (void)Align;
  return createStringError(inconvertibleErrorCode(),
                           "HostMmapCodeAllocator is POSIX-only");
#else
  if (Size == 0)
    return createStringError(inconvertibleErrorCode(),
                             "HostMmapCodeAllocator: zero-size allocation");
  long PageSizeL = ::sysconf(_SC_PAGESIZE);
  std::size_t PageSize = PageSizeL > 0 ? (std::size_t)PageSizeL : 4096u;
  if (Align < 16)
    Align = 16;
  // 申请 Size 向上取整到页，外加一页冗余，保证对齐后仍有 Size 空间。
  std::size_t MapLen = ((Size + Align + PageSize - 1) / PageSize) * PageSize;
  void *Base = ::mmap(nullptr, MapLen, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Base == MAP_FAILED)
    return createStringError(inconvertibleErrorCode(),
                             "HostMmapCodeAllocator: mmap failed");
  uintptr_t Raw = reinterpret_cast<uintptr_t>(Base);
  uintptr_t Aligned = (Raw + (Align - 1)) & ~((uintptr_t)Align - 1);
  uint8_t *User = reinterpret_cast<uint8_t *>(Aligned);

  Region R;
  R.Base = Base;
  R.MapLen = MapLen;
  R.User = User;
  R.UserLen = Size;
  Regions.push_back(R);
  return MutableArrayRef<uint8_t>(User, Size);
#endif
}

Error HostMmapCodeAllocator::finalize(MutableArrayRef<uint8_t> Code) {
#if defined(_WIN32)
  (void)Code;
  return createStringError(inconvertibleErrorCode(),
                           "HostMmapCodeAllocator is POSIX-only");
#else
  // 找到包含 Code 的 region，按整页切换权限。
  for (auto &R : Regions) {
    if (R.User == Code.data()) {
      // 刷新指令缓存（D-cache clean + I-cache invalidate）后再切 RX。
      __builtin___clear_cache(reinterpret_cast<char *>(R.User),
                              reinterpret_cast<char *>(R.User) + Code.size());
      if (::mprotect(R.Base, R.MapLen, PROT_READ | PROT_EXEC) != 0)
        return createStringError(inconvertibleErrorCode(),
                                 "HostMmapCodeAllocator: mprotect RX failed");
      return Error::success();
    }
  }
  return createStringError(inconvertibleErrorCode(),
                           "HostMmapCodeAllocator: finalize unknown region");
#endif
}

void HostMmapCodeAllocator::release(MutableArrayRef<uint8_t> Code) {
#if !defined(_WIN32)
  for (auto It = Regions.begin(); It != Regions.end(); ++It) {
    if (It->User == Code.data()) {
      ::munmap(It->Base, It->MapLen);
      Regions.erase(It);
      return;
    }
  }
#else
  (void)Code;
#endif
}

//===----------------------------------------------------------------------===//
// StaticSlabCodeAllocator
//===----------------------------------------------------------------------===//

Expected<MutableArrayRef<uint8_t>>
StaticSlabCodeAllocator::allocate(std::size_t Size, std::size_t Align) {
  if (!Base)
    return createStringError(inconvertibleErrorCode(),
                             "StaticSlabCodeAllocator: null slab");
  if (Align < 16)
    Align = 16;
  std::size_t Aligned = (Offset + (Align - 1)) & ~(Align - 1);
  if (Aligned + Size > Capacity)
    return createStringError(inconvertibleErrorCode(),
                             "StaticSlabCodeAllocator: slab exhausted");
  uint8_t *P = Base + Aligned;
  Offset = Aligned + Size;
  return MutableArrayRef<uint8_t>(P, Size);
}

Error StaticSlabCodeAllocator::finalize(MutableArrayRef<uint8_t> Code) {
  // 先刷 icache（若注入了 hook），再切可执行权限（若注入了 hook）。
  // 两者都没有时为 no-op —— 假定 slab 自身已是 RWX（典型裸核场景）。
  if (Flush)
    Flush(Code.data(), Code.data() + Code.size());
  if (MakeExec) {
    if (MakeExec(Code.data(), Code.size()) != 0)
      return createStringError(inconvertibleErrorCode(),
                               "StaticSlabCodeAllocator: MakeExecutable hook "
                               "failed");
  }
  return Error::success();
}

void StaticSlabCodeAllocator::release(MutableArrayRef<uint8_t> Code) {
  // bump 分配器不支持单块释放；只能整体 reset()。
  (void)Code;
}
