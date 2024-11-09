/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "RegexDefs.h"
#include <AK/StringView.h>
#include <AK/Types.h>

namespace regex {

enum class Error : u8 {
    NoError = __Regex_NoError,
    InvalidPattern = __Regex_InvalidPattern,                           // Invalid regular expression.
    InvalidCollationElement = __Regex_InvalidCollationElement,         // Invalid collating element referenced.
    InvalidCharacterClass = __Regex_InvalidCharacterClass,             // Invalid character class type referenced.
    InvalidTrailingEscape = __Regex_InvalidTrailingEscape,             // Trailing \ in pattern.
    InvalidNumber = __Regex_InvalidNumber,                             // Number in \digit invalid or in error.
    MismatchingBracket = __Regex_MismatchingBracket,                   // [ ] imbalance.
    MismatchingParen = __Regex_MismatchingParen,                       // ( ) imbalance.
    MismatchingBrace = __Regex_MismatchingBrace,                       // { } imbalance.
    InvalidBraceContent = __Regex_InvalidBraceContent,                 // Content of {} invalid: not a number, number too large, more than two numbers, first larger than second.
    InvalidBracketContent = __Regex_InvalidBracketContent,             // Content of [] invalid.
    InvalidRange = __Regex_InvalidRange,                               // Invalid endpoint in range expression.
    InvalidRepetitionMarker = __Regex_InvalidRepetitionMarker,         // ?, * or + not preceded by valid regular expression.
    ReachedMaxRecursion = __Regex_ReachedMaxRecursion,                 // MaximumRecursion has been reached.
    EmptySubExpression = __Regex_EmptySubExpression,                   // Sub expression has empty content.
    InvalidCaptureGroup = __Regex_InvalidCaptureGroup,                 // Content of capture group is invalid.
    InvalidNameForCaptureGroup = __Regex_InvalidNameForCaptureGroup,   // Name of capture group is invalid.
    InvalidNameForProperty = __Regex_InvalidNameForProperty,           // Name of property is invalid.
    DuplicateNamedCapture = __Regex_DuplicateNamedCapture,             // Name of property is invalid.
    InvalidCharacterClassEscape = __Regex_InvalidCharacterClassEscape, // Invalid escaped entity in character class.
};

inline StringView get_error_string(Error error)
{
    switch (error) {
    case Error::NoError:
        return "No error"sv;
    case Error::InvalidPattern:
        return "Invalid regular expression."sv;
    case Error::InvalidCollationElement:
        return "Invalid collating element referenced."sv;
    case Error::InvalidCharacterClass:
        return "Invalid character class type referenced."sv;
    case Error::InvalidTrailingEscape:
        return "Trailing \\ in pattern."sv;
    case Error::InvalidNumber:
        return "Number in \\digit invalid or in error."sv;
    case Error::MismatchingBracket:
        return "[ ] imbalance."sv;
    case Error::MismatchingParen:
        return "( ) imbalance."sv;
    case Error::MismatchingBrace:
        return "{ } imbalance."sv;
    case Error::InvalidBraceContent:
        return "Content of {} invalid: not a number, number too large, more than two numbers, first larger than second."sv;
    case Error::InvalidBracketContent:
        return "Content of [] invalid."sv;
    case Error::InvalidRange:
        return "Invalid endpoint in range expression."sv;
    case Error::InvalidRepetitionMarker:
        return "?, * or + not preceded by valid regular expression."sv;
    case Error::ReachedMaxRecursion:
        return "Maximum recursion has been reached."sv;
    case Error::EmptySubExpression:
        return "Sub expression has empty content."sv;
    case Error::InvalidCaptureGroup:
        return "Content of capture group is invalid."sv;
    case Error::InvalidNameForCaptureGroup:
        return "Name of capture group is invalid."sv;
    case Error::InvalidNameForProperty:
        return "Name of property is invalid."sv;
    case Error::DuplicateNamedCapture:
        return "Duplicate capture group name"sv;
    case Error::InvalidCharacterClassEscape:
        return "Invalid escaped entity in character class."sv;
    }
    return "Undefined error."sv;
}
}

using regex::get_error_string;
