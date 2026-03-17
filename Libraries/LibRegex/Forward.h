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

class RegexStringView;

}

using regex::ECMA262Parser;
using regex::Lexer;
using regex::PosixExtendedParser;
using regex::RegexStringView;
