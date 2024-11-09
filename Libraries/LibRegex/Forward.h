/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace regex {
struct CompareTypeAndValuePair;

enum class Error : u8;
class Lexer;
class PosixExtendedParser;
class ECMA262Parser;

class ByteCode;
class OpCode;
class OpCode_Exit;
class OpCode_Jump;
class OpCode_ForkJump;
class OpCode_ForkStay;
class OpCode_CheckBegin;
class OpCode_CheckEnd;
class OpCode_SaveLeftCaptureGroup;
class OpCode_SaveRightCaptureGroup;
class OpCode_SaveLeftNamedCaptureGroup;
class OpCode_SaveNamedLeftCaptureGroup;
class OpCode_SaveRightNamedCaptureGroup;
class OpCode_Compare;
class RegexStringView;
}

using regex::ECMA262Parser;
using regex::Lexer;
using regex::PosixExtendedParser;
using regex::RegexStringView;
