/*
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibRegex/Regex.h>
#include <LibRegex/RegexOptions.h>
#include <LibRegex/RegexParser.h>

// Create readable PosixExtended regex patterns, recursively.
// Supports abstract regex operations: concatenation, disjunction, repetition, start, end.
// NOTE: We are not using regex::ECMA262, because it does not support duplicate named capturing groups.
class RegexPattern {
protected:
    // Operator precedence (low to high) in the outermost production of the RegexPattern.
    // https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap09.html#tag_09_04_08
    enum Precedence {
        Disjunction,    // The outermost production is a disjunction | of elements.
        Anchor,         // The outermost production contains anchors ^ $.
        Concatentation, // The outermost production is a concatenation of elements.
        Character       // Behaves like a single character (default).
    };

public:
    // Create a RegexPattern from a string.
    RegexPattern(String const& str, Precedence precedence = Precedence::Character)
        : m_string(str)
        , m_precedence(precedence)
    {
    }

    ALWAYS_INLINE RegexResult match(StringView const& string, regex::PosixFlags flags = regex::PosixFlags::Insensitive) const
    {
        // FIXME: Move this to regex::ECMA262 once it supports duplicate named capturing groups.
        // https://github.com/tc39/proposal-duplicate-named-capturing-groups
        Regex<regex::PosixExtended> regex(m_string.to_byte_string(), flags);

        // Do not throw an exception if the regex cannot be parsed.
        // regex.search below will fail silently.
        if (regex.matcher == nullptr)
            warnln("Cannot parse regular expression.");

        RegexResult result;
        regex.search(string, result);
        return result;
    }

    String const& string() const
    {
        return m_string;
    }

    // Enclose the current pattern in a named capture group (PosixExtended).
    // Uses a single alphabetic character for the group name; makes it fast to detect in a switch... case
    RegexPattern group(char group_name) const
    {
        return RegexPattern { String::formatted("(?<{}>{})", group_name, m_string).value() };
    }
    // Enclose the current pattern in a named capture group. The group name must follow identifier rules.
    // Use this if you care about the readability of the resulting regex string,
    // but don't do runtime matching of group names or don't care about its performance.
    RegexPattern group(String const& group_name) const
    {
        return RegexPattern { String::formatted("(?<{}>{})", group_name, m_string).value() };
    }

    // Creates an anonymous, uncapturing group. Use this to force regex operator precedence.
    RegexPattern group() const
    {
        return RegexPattern { String::formatted("(?:{})", m_string).value() };
    }

    // Concatenantes two RegexPattern-s
    virtual RegexPattern operator+(RegexPattern const& other) const
    { // Concatenation
        String s1 = this->m_string;
        String s2 = other.m_string;

        if (this->m_precedence < Concatentation)
            s1 = String::formatted("(?:{})", m_string).value();
        if (other.m_precedence < Concatentation)
            s2 = String::formatted("(?:{})", other.m_string).value();

        return RegexPattern { String::formatted("{}{}", s1, s2).value(), Concatentation };
    }

    // Disjunction (or) between RegexPattern-s
    template<typename... Patterns>
    friend RegexPattern one_of(Patterns const&... patterns)
    {
        return RegexPattern { String::formatted("{}", _one_of(patterns...).string()).value(), Disjunction };
    }
    template<typename... Patterns>
    friend RegexPattern _one_of(RegexPattern const& pattern, Patterns const&... others)
    {
        return RegexPattern { String::formatted("{}|{}", pattern.string(), _one_of(others...).string()).value() };
    }
    friend RegexPattern _one_of(RegexPattern const& pattern)
    {
        return RegexPattern { String::formatted("{}", pattern.string()).value() };
    }

    // Zero or one occurrence of the enclosed pattern.
    static RegexPattern maybe(RegexPattern const& pattern)
    {
        if (pattern.m_precedence < Character)
            return RegexPattern { String::formatted("(?:{})?", pattern.m_string).value() };

        return RegexPattern { String::formatted("{}?", pattern.m_string).value() };
    }
    // Zero or more times (Kleene star).
    RegexPattern star() const
    {
        if (m_precedence < Character)
            return RegexPattern { String::formatted("(?:{})*", m_string).value() };

        return RegexPattern { String::formatted("{}*", m_string).value() };
    }
    // One or more times.
    RegexPattern plus() const
    {
        if (m_precedence < Character)
            return RegexPattern { String::formatted("(?:{})+", m_string).value() };

        return RegexPattern { String::formatted("{}+", m_string).value() };
    }

    // Repetition of the enclosed RegexPattern.
    RegexPattern repeat(size_t count) const
    {
        if (m_precedence < Character)
            return RegexPattern { String::formatted("(?:{}){{{}}}", m_string, count).value() };

        return RegexPattern { String::formatted("{}{{{}}}", m_string, count).value() };
    }
    // Repetition (min/max) of the enclosed RegexPattern.
    RegexPattern repeat(size_t min_count, size_t max_count) const
    {
        if (m_precedence < Character)
            return RegexPattern { String::formatted("(?:{}){{{},{}}}", m_string, min_count, max_count).value() };

        return RegexPattern { String::formatted("{}{{{},{}}}", m_string, min_count, max_count).value() };
    }

    // Add an end marker ($).
    RegexPattern last() const
    {
        if (m_precedence < Anchor)
            return RegexPattern { String::formatted("(?:{})$", m_string).value(), Anchor };

        return RegexPattern { String::formatted("{}$", m_string).value(), Anchor };
    }
    // Add a start marker (^).
    RegexPattern first() const
    {
        if (m_precedence < Anchor)
            return RegexPattern { String::formatted("^(?:{})", m_string).value(), Anchor };

        return RegexPattern { String::formatted("^{}", m_string).value(), Anchor };
    }

    // Encloses a RegexPattern matching a whole string.
    friend RegexPattern full(RegexPattern const& pattern)
    {
        if (pattern.m_precedence < Anchor)
            return RegexPattern { String::formatted("^(?:{})$", pattern.m_string).value(), Anchor };

        return RegexPattern { String::formatted("^{}$", pattern.m_string).value(), Anchor };
    }

    bool operator==(RegexPattern const& other) const
    {
        return this->m_string == other.m_string;
    }

    // Convert a capture group to a number.
    template<Arithmetic T>
    ALWAYS_INLINE static Optional<T> number(regex::Match const& group)
    {
        if (!group.view.is_string_view() || group.view.to_string().is_error())
            return T {};
        return group.view.to_string().value().to_number<T>();
    }
    // Extract the first character of the name of a capture group.
    ALWAYS_INLINE static Optional<char> group_name_first_char(regex::Match const& group)
    {
        if (!group.capture_group_name.has_value())
            return {};

        return group.capture_group_name.value().characters()[0];
    }
    // Whether a capture group starts with a given character
    ALWAYS_INLINE static bool does_group_start_with_char(regex::Match const& group, char c)
    {
        if (!group.view.is_string_view() || group.view.to_string().is_error() || (group.view.to_byte_string().length() > 1))
            return false;
        return group.view.to_byte_string().characters()[0] == c;
    }

    // Useful for parsing numbers with variable length decimals, e.g. nanosecond decimals
    // Example decimals=9: .123 --> 123000000 .123456 -> 123456000
    template<typename T>
    ALWAYS_INLINE static Optional<T> string_to_decimals(StringView const& string, size_t decimals)
    {
        AK::StringBuilder sb;
        AK::FormatBuilder fb(sb);
        if (fb.put_string(string, AK::FormatBuilder::Align::Left, decimals, decimals, '0').is_error())
            return {};

        return fb.builder().to_string().release_value().to_number<T>();
    }

private:
    String const m_string;
    Precedence const m_precedence;
};
