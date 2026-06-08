// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/runtime/instance/table.h - Table Instance definition -----===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the table instance definition in store manager.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "ast/segment.h"
#include "ast/type.h"
#include "common/errcode.h"
#include "common/errinfo.h"
#include "common/spdlog.h"
#include "gc/allocator.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace WasmEdge {
namespace Runtime {
namespace Instance {

class TableInstance {
public:
  TableInstance() = delete;
  TableInstance(const AST::TableType &TType) noexcept
      : TabType(TType),
        Refs(TType.getLimit().getMin(), RefVariant(TType.getRefType())),
        InitValue(RefVariant(TType.getRefType())) {
    // The reference type should be nullable because there is no initial ref.
    // This constructor only handles abstract heap types correctly for null
    // refs. For concrete type indices, the caller should use the two-arg
    // constructor with a properly initialized RefVariant.
    assuming(TType.getRefType().isNullableRefType());
    assuming(TType.getRefType().isAbsHeapType());
  }
  TableInstance(const AST::TableType &TType, const RefVariant &InitVal) noexcept
      : TabType(TType), Refs(TType.getLimit().getMin(), InitVal),
        InitValue(InitVal) {
    // If the reference type is not nullable, the initial reference is required.
    assuming(TType.getRefType().isNullableRefType() || !InitVal.isNull());
  }

  ~TableInstance() noexcept {
    if (Allocator) {
      Allocator->removeTable(*this);
    }
  }

  void setAllocator(GC::Allocator &A) noexcept {
    assuming(Allocator == nullptr);
    Allocator = &A;
    Allocator->addTable(*this);
  }

  /// Get size of table.refs
  uint64_t getSize() const noexcept {
    // The table size is bound to the limit in the table type.
    return TabType.getLimit().getMin();
  }

  /// Getter for table type.
  const AST::TableType &getTableType() const noexcept { return TabType; }

  /// Check whether access is out of bounds.
  bool checkAccessBound(const uint64_t Offset,
                        const uint64_t Length) const noexcept {
    // Due to applying the Memory64 proposal, we should avoid the overflow issue
    // of the following code:
    //   return Offset + Length <= Limit;
    const uint64_t Limit = TabType.getLimit().getMin();
    return std::numeric_limits<uint64_t>::max() - Offset >= Length &&
           Offset + Length <= Limit;
  }

  /// Grow table with initialization value.
  bool growTable(const uint64_t Count, const RefVariant &Val) noexcept {
    if (Count == 0) {
      return true;
    }
    uint64_t MaxSizeCaped = getMaxAddress(TabType.getLimit().getAddrType());
    const uint64_t Min = TabType.getLimit().getMin();
    assuming(MaxSizeCaped >= Min);
    if (TabType.getLimit().hasMax()) {
      const uint64_t Max = TabType.getLimit().getMax();
      MaxSizeCaped = std::min(Max, MaxSizeCaped);
    }
    if (Count > MaxSizeCaped - Min) {
      return false;
    }
    Refs.resize(Refs.size() + Count);
    std::fill_n(Refs.end() - static_cast<std::ptrdiff_t>(Count), Count, Val);
    // The new slots join this table's GC roots after the root snapshot; shade
    // the broadcast reference so a concurrent collection keeps it alive.
    if (Allocator) {
      Allocator->writeBarrier(Val);
    }
    TabType.getLimit().setMin(Min + Count);
    return true;
  }
  bool growTable(const uint64_t Count) noexcept {
    return growTable(Count, InitValue);
  }

  /// Get slice of Refs[Offset : Offset + Length - 1]
  Expect<Span<const RefVariant>> getRefs(const uint64_t Offset,
                                         const uint64_t Length) const noexcept {
    // Check the accessing boundary.
    if (!checkAccessBound(Offset, Length)) {
      spdlog::error(ErrCode::Value::TableOutOfBounds);
      spdlog::error(ErrInfo::InfoBoundary(Offset, Length, getSize()));
      return Unexpect(ErrCode::Value::TableOutOfBounds);
    }
    return Span<const RefVariant>(Refs).subspan(Offset, Length);
  }

  /// Replace the Refs[Dst :] by Slice.
  Expect<void> setRefs(Span<const RefVariant> Slice,
                       const uint64_t Dst) noexcept {
    const uint64_t Length = static_cast<uint64_t>(Slice.size());
    // Check the accessing boundary.
    if (!checkAccessBound(Dst, Length)) {
      spdlog::error(ErrCode::Value::TableOutOfBounds);
      spdlog::error(ErrInfo::InfoBoundary(Dst, Length, getSize()));
      return Unexpect(ErrCode::Value::TableOutOfBounds);
    }

    auto Write = Span<RefVariant>(Refs).subspan(Dst, Slice.size());

    // Table slots are GC roots; shade both the overwritten and the newly
    // stored references so a concurrent collection started before this write
    // does not reclaim a still-reachable object.
    if (Allocator) {
      Allocator->bulkWriteBarrier(Span<const RefVariant>(Write));
      Allocator->bulkWriteBarrier(Slice);
    }

    // Copy the references. Slice may alias this table's storage, so detect a
    // genuine overlap using integer addresses; relational comparison of
    // pointers from potentially distinct arrays would be unspecified behavior.
    const auto WriteAddr = reinterpret_cast<uintptr_t>(Write.data());
    const auto SliceBeginAddr = reinterpret_cast<uintptr_t>(Slice.data());
    const auto SliceEndAddr =
        reinterpret_cast<uintptr_t>(Slice.data() + Slice.size());
    if (WriteAddr > SliceBeginAddr && WriteAddr < SliceEndAddr) {
      // Destination starts inside the source range: copy backward so earlier
      // writes do not clobber not-yet-read source elements.
      std::copy_backward(Slice.begin(), Slice.end(), Write.end());
    } else {
      std::copy(Slice.begin(), Slice.end(), Write.begin());
    }
    return {};
  }

  /// Fill the Refs[Offset : Offset + Length - 1] by Val.
  Expect<void> fillRefs(const RefVariant &Val, const uint64_t Offset,
                        const uint64_t Length) noexcept {
    // Check the accessing boundary.
    if (!checkAccessBound(Offset, Length)) {
      spdlog::error(ErrCode::Value::TableOutOfBounds);
      spdlog::error(ErrInfo::InfoBoundary(Offset, Length, getSize()));
      return Unexpect(ErrCode::Value::TableOutOfBounds);
    }

    // Table slots are GC roots; shade the overwritten range and the fill value
    // so a concurrent collection does not miss a still-reachable object.
    if (Allocator) {
      Allocator->bulkWriteBarrier(
          Span<const RefVariant>(Refs).subspan(Offset, Length));
      Allocator->writeBarrier(Val);
    }

    // Fill the references.
    std::fill_n(Refs.begin() + static_cast<std::ptrdiff_t>(Offset), Length,
                Val);
    return {};
  }

  /// Get the elem address.
  Expect<RefVariant> getRefAddr(const uint64_t Idx) const noexcept {
    if (Idx >= Refs.size()) {
      spdlog::error(ErrCode::Value::TableOutOfBounds);
      spdlog::error(ErrInfo::InfoBoundary(Idx, 1, getSize()));
      return Unexpect(ErrCode::Value::TableOutOfBounds);
    }
    return Refs[Idx];
  }

  /// Set the elem address.
  Expect<void> setRefAddr(const uint64_t Idx, const RefVariant &Val) {
    if (Idx >= Refs.size()) {
      spdlog::error(ErrCode::Value::TableOutOfBounds);
      spdlog::error(ErrInfo::InfoBoundary(Idx, 1, getSize()));
      return Unexpect(ErrCode::Value::TableOutOfBounds);
    }
    // Table slots are GC roots; shade both the overwritten and the newly stored
    // reference so a concurrent collection does not miss a reachable object.
    if (Allocator) {
      Allocator->writeBarrier(Refs[Idx]);
      Allocator->writeBarrier(Val);
    }
    Refs[Idx] = Val;
    return {};
  }

private:
  friend class GC::Allocator;
  void clearAllocator(GC::Allocator &A) noexcept {
    if (Allocator == &A) {
      Allocator = nullptr;
    }
  }

  /// \name Data of table instance.
  /// @{
  GC::Allocator *Allocator = nullptr;
  AST::TableType TabType;
  std::vector<RefVariant> Refs;
  RefVariant InitValue;
  /// @}
};

} // namespace Instance
} // namespace Runtime
} // namespace WasmEdge
