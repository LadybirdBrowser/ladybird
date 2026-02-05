/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibRegex/Export.h>

namespace regex {

struct CompareTypeAndValuePair;

enum class Error : u8;
class Lexer;
class PosixExtendedParser;
class ECMA262Parser;

class ByteCode;
template<typename ByteCode>
class OpCode;
template<typename ByteCode>
class OpCode_Exit;
template<typename ByteCode>
class OpCode_Jump;
template<typename ByteCode>
class OpCode_ForkJump;
template<typename ByteCode>
class OpCode_ForkStay;
template<typename ByteCode>
class OpCode_CheckBegin;
template<typename ByteCode>
class OpCode_CheckEnd;
template<typename ByteCode>
class OpCode_SaveLeftCaptureGroup;
template<typename ByteCode>
class OpCode_SaveRightCaptureGroup;
template<typename ByteCode>
class OpCode_SaveLeftNamedCaptureGroup;
template<typename ByteCode>
class OpCode_SaveNamedLeftCaptureGroup;
template<typename ByteCode>
class OpCode_SaveRightNamedCaptureGroup;
template<typename ByteCode>
class OpCode_Compare;

class RegexStringView;

}

using regex::ECMA262Parser;
using regex::Lexer;
using regex::PosixExtendedParser;
using regex::RegexStringView;
