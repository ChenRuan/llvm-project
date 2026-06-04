//===- EJitLightCodeAllocator.h - light backend code memory abstraction ---===//
//
// EmbeddedJIT 轻量后端的可执行代码内存抽象。
//
// 发射器 (EJitLightAArch64) 只会往一段普通可写缓冲区里写指令字节，绝不
// 自己 mmap/mprotect。可执行内存的申请、权限切换、icache 刷新、释放都
// 由 CodeAllocator 的实现负责。这样：
//   * host Linux 测试用 HostMmapCodeAllocator（mmap RW -> 写入 ->
//     __builtin___clear_cache -> mprotect RX）。
//   * 裸核 / freestanding 用 StaticSlabCodeAllocator（在一段预留 buffer
//     里 bump 分配，权限切换是 no-op 或由注入的 hook 完成），不调用任何
//     POSIX API，也不绑定任何 SRE 私有接口。
//
// 注意：这里刻意不引入 SRE_MmuMap / SRE_MemDbgAlloc 等私有符号。裸核接入
// 方应当自己实现一个 CodeAllocator 子类，或给 StaticSlabCodeAllocator
// 注入 FlushICacheFn / MakeExecutableFn 钩子。
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTCODEALLOCATOR_H
#define LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTCODEALLOCATOR_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace llvm {
namespace ejit {
namespace light {

// 裸核接入方可注入的钩子类型。
//   FlushICacheFn   : 刷新 [begin, end) 的指令缓存。
//   MakeExecutableFn: 把 [base, base+size) 切成可执行权限，返回 0 表示成功。
// 它们刻意只用裸指针 + size，不绑定任何平台私有接口。
using FlushICacheFn = void (*)(void *Begin, void *End);
using MakeExecutableFn = int (*)(void *Base, std::size_t Size);

// 代码内存分配器抽象。
//
// 典型使用顺序：
//   auto Buf = Alloc.allocate(Size, Align);   // 取一段可写缓冲区
//   ... 发射器往 Buf 写指令字节 ...
//   Alloc.finalize(Buf);                       // 刷 icache + 切 RX
//   // Buf.data() 现在可作为函数指针调用
//   ...
//   Alloc.release(Buf);                        // 回收（可能是 no-op）
class CodeAllocator {
public:
  virtual ~CodeAllocator() = default;

  // 申请一段至少 Size 字节、按 Align 对齐的可写内存。失败返回 Error。
  virtual Expected<MutableArrayRef<uint8_t>> allocate(std::size_t Size,
                                                      std::size_t Align) = 0;

  // 把 Code 这段已经写好指令的缓冲区定稿：刷新 icache、切换为可执行权限。
  // 失败返回 Error。
  virtual Error finalize(MutableArrayRef<uint8_t> Code) = 0;

  // 回收 Code。对某些分配器（StaticSlab）可能是 no-op。
  virtual void release(MutableArrayRef<uint8_t> Code) = 0;
};

// ---------------------------------------------------------------------------
// HostMmapCodeAllocator —— 用于 Linux host 测试。
//
//   allocate : mmap(PROT_READ|PROT_WRITE) 一段页对齐内存。
//   finalize : __builtin___clear_cache + mprotect(PROT_READ|PROT_EXEC)。
//   release  : munmap。
//
// 仅在 host（有 POSIX mmap）上可用。裸核请勿使用本类。
// ---------------------------------------------------------------------------
class HostMmapCodeAllocator final : public CodeAllocator {
public:
  HostMmapCodeAllocator() = default;
  ~HostMmapCodeAllocator() override;

  Expected<MutableArrayRef<uint8_t>> allocate(std::size_t Size,
                                              std::size_t Align) override;
  Error finalize(MutableArrayRef<uint8_t> Code) override;
  void release(MutableArrayRef<uint8_t> Code) override;

private:
  struct Region {
    void *Base = nullptr;     // mmap 返回的页基址
    std::size_t MapLen = 0;   // mmap 的总长度（页对齐）
    uint8_t *User = nullptr;  // 返回给调用方的对齐起始地址
    std::size_t UserLen = 0;  // 返回给调用方的长度
  };
  // 简单线性表即可：light 后端一次只编译一个函数，region 数量极少。
  std::vector<Region> Regions;
};

// ---------------------------------------------------------------------------
// StaticSlabCodeAllocator —— 用于 freestanding / 裸核原型。
//
// 构造时传入一段预留 buffer（uint8_t* + size，或 MutableArrayRef）。在该
// buffer 内 bump 分配，不调用任何 POSIX API。
//   finalize : 若注入了 FlushICacheFn 则调用之；若注入了 MakeExecutableFn
//              则调用之；否则为 no-op（假定该 slab 本身已是 RWX，比如某些
//              裸核场景）。
//   release  : no-op（只能整体 reset）。
// ---------------------------------------------------------------------------
class StaticSlabCodeAllocator final : public CodeAllocator {
public:
  StaticSlabCodeAllocator(uint8_t *Base, std::size_t Size)
      : Base(Base), Capacity(Size) {}
  explicit StaticSlabCodeAllocator(MutableArrayRef<uint8_t> Slab)
      : Base(Slab.data()), Capacity(Slab.size()) {}

  Expected<MutableArrayRef<uint8_t>> allocate(std::size_t Size,
                                              std::size_t Align) override;
  Error finalize(MutableArrayRef<uint8_t> Code) override;
  void release(MutableArrayRef<uint8_t> Code) override;

  // 注入裸核钩子。两者都可为 nullptr。
  void setFlushICache(FlushICacheFn Fn) { Flush = Fn; }
  void setMakeExecutable(MakeExecutableFn Fn) { MakeExec = Fn; }

  // 把整段 slab 重置为可重新分配。
  void reset() { Offset = 0; }

  std::size_t used() const { return Offset; }
  std::size_t capacity() const { return Capacity; }

private:
  uint8_t *Base = nullptr;
  std::size_t Capacity = 0;
  std::size_t Offset = 0;
  FlushICacheFn Flush = nullptr;
  MakeExecutableFn MakeExec = nullptr;
};

} // namespace light
} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTCODEALLOCATOR_H
