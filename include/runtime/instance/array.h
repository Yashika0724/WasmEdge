// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/runtime/instance/array.h - Array Instance definition -----===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the array instance definition in store manager.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "ast/type.h"
#include "common/span.h"
#include "common/types.h"
#include "gc/allocator.h"
#include "runtime/instance/gc.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace WasmEdge::Runtime::Instance {

class ArrayInstance : public GCInstance {
public:
  ArrayInstance() = delete;
  ArrayInstance(GC::Allocator &Allocator, const ModuleInstance *ModInst,
                uint32_t TypeIdx, const uint32_t Size,
                const ValVariant &Init) noexcept {
    assuming(Size <= (std::numeric_limits<uint32_t>::max() - sizeof(RawData)) /
                         sizeof(ValVariant));
    Data = static_cast<RawData *>(Allocator.allocate(
        [&](void *Pointer) {
          auto Raw = static_cast<RawData *>(Pointer);
          Raw->ModInst = ModInst;
          Raw->TypeIdx = TypeIdx;
          Raw->Length = Size;
          std::fill(Raw->Data, Raw->Data + Size, Init);
        },
        static_cast<uint32_t>(sizeof(RawData) + Size * sizeof(ValVariant))));
  }
  ArrayInstance(GC::Allocator &Allocator, const ModuleInstance *ModInst,
                uint32_t TypeIdx, std::vector<ValVariant> &&Init) noexcept {
    assuming(Init.size() <=
             (std::numeric_limits<uint32_t>::max() - sizeof(RawData)) /
                 sizeof(ValVariant));
    Data = static_cast<RawData *>(Allocator.allocate(
        [&](void *Pointer) {
          auto Raw = static_cast<RawData *>(Pointer);
          Raw->ModInst = ModInst;
          Raw->TypeIdx = TypeIdx;
          Raw->Length = static_cast<uint32_t>(Init.size());
          std::copy(Init.begin(), Init.end(), Raw->Data);
        },
        static_cast<uint32_t>(sizeof(RawData) +
                              Init.size() * sizeof(ValVariant))));
  }
  ArrayInstance(RawData *Raw) noexcept : GCInstance(Raw) {}

  /// Get field data in array instance.
  /// Data may be null only on allocation failure, which arrayNew() turns into a
  /// trap before any instance reaches these accessors; callers reach them only
  /// after a null-reference check.
  ValVariant &getData(uint32_t Idx) noexcept {
    assuming(Data);
    return Data->Data[Idx];
  }
  const ValVariant &getData(uint32_t Idx) const noexcept {
    assuming(Data);
    return Data->Data[Idx];
  }

  /// Get full array.
  Span<ValVariant> getArray() noexcept {
    assuming(Data);
    return {Data->Data, Data->Length};
  }
  Span<const ValVariant> getArray() const noexcept {
    assuming(Data);
    return {Data->Data, Data->Length};
  }

  /// Get array length.
  uint32_t getLength() const noexcept {
    assuming(Data);
    return Data->Length;
  }
};

} // namespace WasmEdge::Runtime::Instance
