/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "RegexDefs.h"
#include <AK/Types.h>
#include <stdio.h>

namespace regex {

using FlagsUnderlyingType = u32;

enum class AllFlags {
    Default = 0,
    Global = __Regex_Global,                                             // All matches (don't return after first match)
    Insensitive = __Regex_Insensitive,                                   // Case insensitive match (ignores case of [a-zA-Z])
    Ungreedy = __Regex_Ungreedy,                                         // The match becomes lazy by default. Now a ? following a quantifier makes it greedy
    Unicode = __Regex_Unicode,                                           // Enable all unicode features and interpret all unicode escape sequences as such
    Extended = __Regex_Extended,                                         // Ignore whitespaces. Spaces and text after a # in the pattern are ignored
    Extra = __Regex_Extra,                                               // Disallow meaningless escapes. A \ followed by a letter with no special meaning is faulted
    MatchNotBeginOfLine = __Regex_MatchNotBeginOfLine,                   // Pattern is not forced to ^ -> search in whole string!
    MatchNotEndOfLine = __Regex_MatchNotEndOfLine,                       // Don't Force the dollar sign, $, to always match end of the string, instead of end of the line. This option is ignored if the Multiline-flag is set
    SkipSubExprResults = __Regex_SkipSubExprResults,                     // Do not return sub expressions in the result
    SingleLine = __Regex_SingleLine,                                     // Dot matches newline characters
    Sticky = __Regex_Sticky,                                             // Force the pattern to only match consecutive matches from where the previous match ended.
    Multiline = __Regex_Multiline,                                       // Handle newline characters. Match each line, one by one.
    SkipTrimEmptyMatches = __Regex_SkipTrimEmptyMatches,                 // Do not remove empty capture group results.
    SingleMatch = __Regex_SingleMatch,                                   // Stop after acquiring a single match.
    UnicodeSets = __Regex_UnicodeSets,                                   // Only for ECMA262, Allow set operations in character classes.
    Internal_Stateful = __Regex_Internal_Stateful,                       // Make global matches match one result at a time, and further match() calls on the same instance continue where the previous one left off.
    Internal_BrowserExtended = __Regex_Internal_BrowserExtended,         // Only for ECMA262, Enable the behaviors defined in section B.1.4. of the ECMA262 spec.
    Internal_ConsiderNewline = __Regex_Internal_ConsiderNewline,         // Only for ECMA262, Allow multiline matches to consider newlines as line boundaries.
    Internal_ECMA262DotSemantics = __Regex_Internal_ECMA262DotSemantics, // Use ECMA262 dot semantics: disallow matching CR/LF/LS/PS instead of just CR.
    Last = Internal_BrowserExtended,
};

enum class PosixFlags : FlagsUnderlyingType {
    Default = 0,
    Global = (FlagsUnderlyingType)AllFlags::Global,
    Insensitive = (FlagsUnderlyingType)AllFlags::Insensitive,
    Ungreedy = (FlagsUnderlyingType)AllFlags::Ungreedy,
    Unicode = (FlagsUnderlyingType)AllFlags::Unicode,
    Extended = (FlagsUnderlyingType)AllFlags::Extended,
    Extra = (FlagsUnderlyingType)AllFlags::Extra,
    MatchNotBeginOfLine = (FlagsUnderlyingType)AllFlags::MatchNotBeginOfLine,
    MatchNotEndOfLine = (FlagsUnderlyingType)AllFlags::MatchNotEndOfLine,
    SkipSubExprResults = (FlagsUnderlyingType)AllFlags::SkipSubExprResults,
    SkipTrimEmptyMatches = (FlagsUnderlyingType)AllFlags::SkipTrimEmptyMatches,
    Multiline = (FlagsUnderlyingType)AllFlags::Multiline,
    SingleMatch = (FlagsUnderlyingType)AllFlags::SingleMatch,
};

enum class ECMAScriptFlags : FlagsUnderlyingType {
    Default = (FlagsUnderlyingType)AllFlags::Internal_ECMA262DotSemantics,
    Global = (FlagsUnderlyingType)AllFlags::Global | (FlagsUnderlyingType)AllFlags::Internal_Stateful, // Note: ECMAScript "Global" creates a stateful regex.
    Insensitive = (FlagsUnderlyingType)AllFlags::Insensitive,
    Ungreedy = (FlagsUnderlyingType)AllFlags::Ungreedy,
    Unicode = (FlagsUnderlyingType)AllFlags::Unicode,
    Extended = (FlagsUnderlyingType)AllFlags::Extended,
    Extra = (FlagsUnderlyingType)AllFlags::Extra,
    SingleLine = (FlagsUnderlyingType)AllFlags::SingleLine,
    Sticky = (FlagsUnderlyingType)AllFlags::Sticky,
    Multiline = (FlagsUnderlyingType)AllFlags::Multiline,
    UnicodeSets = (FlagsUnderlyingType)AllFlags::UnicodeSets,
    BrowserExtended = (FlagsUnderlyingType)AllFlags::Internal_BrowserExtended,
};

template<class T>
class RegexOptions {
public:
    using FlagsType = T;

    RegexOptions() = default;

    constexpr RegexOptions(T flags)
        : m_flags(static_cast<T>(to_underlying(flags) | to_underlying(T::Default)))
    {
    }

    template<class U>
    constexpr RegexOptions(RegexOptions<U> other)
        : RegexOptions(static_cast<T>(to_underlying(other.value())))
    {
    }

    operator bool() const { return !!*this; }
    bool operator!() const { return (FlagsUnderlyingType)m_flags == 0; }

    constexpr RegexOptions<T> operator|(T flag) const { return RegexOptions<T> { (T)((FlagsUnderlyingType)m_flags | (FlagsUnderlyingType)flag) }; }
    constexpr RegexOptions<T> operator&(T flag) const { return RegexOptions<T> { (T)((FlagsUnderlyingType)m_flags & (FlagsUnderlyingType)flag) }; }

    constexpr RegexOptions<T>& operator|=(T flag)
    {
        m_flags = (T)((FlagsUnderlyingType)m_flags | (FlagsUnderlyingType)flag);
        return *this;
    }

    constexpr RegexOptions<T>& operator&=(T flag)
    {
        m_flags = (T)((FlagsUnderlyingType)m_flags & (FlagsUnderlyingType)flag);
        return *this;
    }

    void reset_flags() { m_flags = (T)0; }
    void reset_flag(T flag) { m_flags = (T)((FlagsUnderlyingType)m_flags & ~(FlagsUnderlyingType)flag); }
    void set_flag(T flag) { *this |= flag; }
    bool has_flag_set(T flag) const { return (FlagsUnderlyingType)flag == ((FlagsUnderlyingType)m_flags & (FlagsUnderlyingType)flag); }
    constexpr T value() const { return m_flags; }

private:
    T m_flags { T::Default };
};

template<class T>
constexpr RegexOptions<T> operator|(T lhs, T rhs)
{
    return RegexOptions<T> { lhs } |= rhs;
}

template<class T>
constexpr RegexOptions<T> operator&(T lhs, T rhs)
{
    return RegexOptions<T> { lhs } &= rhs;
}

template<class T>
constexpr T operator~(T flag)
{
    return (T) ~((FlagsUnderlyingType)flag);
}

using AllOptions = RegexOptions<AllFlags>;
using ECMAScriptOptions = RegexOptions<ECMAScriptFlags>;
using PosixOptions = RegexOptions<PosixFlags>;

}

using regex::ECMAScriptFlags;
using regex::ECMAScriptOptions;
using regex::PosixFlags;
using regex::PosixOptions;
