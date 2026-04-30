//===-- NVPTXDataType.h - PTX data types -----------------------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares classes to represent PTX data types.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_NVPTX_NVPTXDATATYPE_H
#define LLVM_LIB_TARGET_NVPTX_NVPTXDATATYPE_H

#include <cassert>

namespace llvm {

/// Class to encapsulate PTX data types as described in -
/// https://docs.nvidia.com/cuda/parallel-thread-execution/#types
// TODO: Expand this class for advanced FP data types.
class NVPTXDataType {
public:
  unsigned BitSize;

  // Fundamental data types in PTX.
  enum class BasicType {
    Unknown,
    Unsigned,
    Signed,
    FloatingPoint,
    Bits,
    Predicate
  };

  BasicType Type;

  NVPTXDataType(unsigned BitSize, BasicType Type)
      : BitSize(BitSize), Type(Type) {
    if (Type == BasicType::Predicate) {
      assert(BitSize == 1 &&
             "PTX predicate data type is expected to be 1-bit sized.");
    }
  }

  // Quick helpers to identify type.
  bool isUnsigned() { return Type == BasicType::Unsigned; }
  bool isSigned() { return Type == BasicType::Signed; }
  bool isFloating() { return Type == BasicType::FloatingPoint; }
  bool isBits() { return Type == BasicType::Bits; }
  bool isPred() { return Type == BasicType::Predicate; }
  bool isUnknown() { return Type == BasicType::Unknown; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_NVPTX_NVPTXDATATYPE_H
