/*
 * Copyright (c) 2022, LI YUBEI <leeight@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibJS/Forward.h>

namespace JS {

// https://github.com/tc39/proposal-regexp-legacy-features#regexp
// The %RegExp% intrinsic object, which is the builtin RegExp constructor, has the following additional internal slots:
// [[RegExpInput]]
// [[RegExpLastMatch]]
// [[RegExpLastParen]]
// [[RegExpLeftContext]]
// [[RegExpRightContext]]
// [[RegExpParen1]] ... [[RegExpParen9]]
class RegExpLegacyStaticProperties {
public:
    Optional<Utf16String> const& input() const { return m_input; }
    Optional<Utf16String> const& last_match() const
    {
        if (!m_last_match_string.has_value())
            m_last_match_string = Utf16String::from_utf16(m_last_match);
        return m_last_match_string;
    }
    Optional<Utf16String> const& last_paren() const { return m_last_paren; }
    Optional<Utf16String> const& left_context() const
    {
        if (!m_left_context_string.has_value())
            m_left_context_string = Utf16String::from_utf16(m_left_context);
        return m_left_context_string;
    }
    Optional<Utf16String> const& right_context() const
    {
        if (!m_right_context_string.has_value())
            m_right_context_string = Utf16String::from_utf16(m_right_context);
        return m_right_context_string;
    }
    Optional<Utf16String> const& $1() const { return lazy_paren(0); }
    Optional<Utf16String> const& $2() const { return lazy_paren(1); }
    Optional<Utf16String> const& $3() const { return lazy_paren(2); }
    Optional<Utf16String> const& $4() const { return lazy_paren(3); }
    Optional<Utf16String> const& $5() const { return lazy_paren(4); }
    Optional<Utf16String> const& $6() const { return lazy_paren(5); }
    Optional<Utf16String> const& $7() const { return lazy_paren(6); }
    Optional<Utf16String> const& $8() const { return lazy_paren(7); }
    Optional<Utf16String> const& $9() const { return lazy_paren(8); }

    void set_input(Utf16String input) { m_input = move(input); }
    void set_last_match(Utf16View last_match)
    {
        m_last_match = last_match;
        m_last_match_string = {};
    }
    void set_last_paren(Utf16String last_paren) { m_last_paren = move(last_paren); }
    void set_left_context(Utf16View left_context)
    {
        m_left_context = left_context;
        m_left_context_string = {};
    }
    void set_right_context(Utf16View right_context)
    {
        m_right_context = right_context;
        m_right_context_string = {};
    }
    void set_$1(Utf16String value)
    {
        m_$[0] = move(value);
        m_parens_materialized = true;
    }
    void set_$2(Utf16String value)
    {
        m_$[1] = move(value);
        m_parens_materialized = true;
    }
    void set_$3(Utf16String value)
    {
        m_$[2] = move(value);
        m_parens_materialized = true;
    }
    void set_$4(Utf16String value)
    {
        m_$[3] = move(value);
        m_parens_materialized = true;
    }
    void set_$5(Utf16String value)
    {
        m_$[4] = move(value);
        m_parens_materialized = true;
    }
    void set_$6(Utf16String value)
    {
        m_$[5] = move(value);
        m_parens_materialized = true;
    }
    void set_$7(Utf16String value)
    {
        m_$[6] = move(value);
        m_parens_materialized = true;
    }
    void set_$8(Utf16String value)
    {
        m_$[7] = move(value);
        m_parens_materialized = true;
    }
    void set_$9(Utf16String value)
    {
        m_$[8] = move(value);
        m_parens_materialized = true;
    }

    // Lazy update: store capture indices without materializing strings.
    // Strings are created on demand when $1-$9 are accessed.
    void set_captures_lazy(size_t num_captures, int const* capture_starts, int const* capture_ends);

    void invalidate();

private:
    Optional<Utf16String> const& lazy_paren(size_t index) const;

    Optional<Utf16String> m_input;
    Optional<Utf16String> m_last_paren;
    mutable Optional<Utf16String> m_$[9];

    // NOTE: These are views into m_input and we only turn them into full strings if/when needed.
    Utf16View m_last_match;
    Utf16View m_left_context;
    Utf16View m_right_context;
    mutable Optional<Utf16String> m_last_match_string;
    mutable Optional<Utf16String> m_left_context_string;
    mutable Optional<Utf16String> m_right_context_string;

    // Lazy capture storage: indices into m_input.
    // -1 means not captured. Strings materialized on access.
    int m_paren_starts[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    int m_paren_ends[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    mutable bool m_parens_materialized = true;
};

ThrowCompletionOr<void> set_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, void (RegExpLegacyStaticProperties::*property_setter)(Utf16String), Value value);
ThrowCompletionOr<Value> get_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, Optional<Utf16String> const& (RegExpLegacyStaticProperties::*property_getter)() const);
void update_legacy_regexp_static_properties(RegExpConstructor& constructor, Utf16String const& string, size_t start_index, size_t end_index, Vector<Utf16String> const& captured_values);
void update_legacy_regexp_static_properties_lazy(RegExpConstructor& constructor, Utf16String const& string, size_t start_index, size_t end_index, size_t num_captures, int const* capture_starts, int const* capture_ends);
void invalidate_legacy_regexp_static_properties(RegExpConstructor& constructor);

}
