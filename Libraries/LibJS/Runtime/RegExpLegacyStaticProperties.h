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
    GC::Ptr<PrimitiveString> input() const { return m_input; }
    GC::Ptr<PrimitiveString> last_match() const;
    GC::Ptr<PrimitiveString> last_paren() const;
    GC::Ptr<PrimitiveString> left_context() const;
    GC::Ptr<PrimitiveString> right_context() const;
    GC::Ptr<PrimitiveString> $1() const { return lazy_paren(0); }
    GC::Ptr<PrimitiveString> $2() const { return lazy_paren(1); }
    GC::Ptr<PrimitiveString> $3() const { return lazy_paren(2); }
    GC::Ptr<PrimitiveString> $4() const { return lazy_paren(3); }
    GC::Ptr<PrimitiveString> $5() const { return lazy_paren(4); }
    GC::Ptr<PrimitiveString> $6() const { return lazy_paren(5); }
    GC::Ptr<PrimitiveString> $7() const { return lazy_paren(6); }
    GC::Ptr<PrimitiveString> $8() const { return lazy_paren(7); }
    GC::Ptr<PrimitiveString> $9() const { return lazy_paren(8); }

    void set_input(GC::Ref<PrimitiveString> input) { m_input = input; }
    void set_match_source(GC::Ref<PrimitiveString>);
    void set_last_match(size_t start, size_t length);
    void set_last_paren(GC::Ref<PrimitiveString>);
    void set_left_context(size_t start, size_t length);
    void set_right_context(size_t start, size_t length);
    void set_$1(GC::Ref<PrimitiveString> value)
    {
        m_$[0] = value;
        m_parens_materialized = true;
    }
    void set_$2(GC::Ref<PrimitiveString> value)
    {
        m_$[1] = value;
        m_parens_materialized = true;
    }
    void set_$3(GC::Ref<PrimitiveString> value)
    {
        m_$[2] = value;
        m_parens_materialized = true;
    }
    void set_$4(GC::Ref<PrimitiveString> value)
    {
        m_$[3] = value;
        m_parens_materialized = true;
    }
    void set_$5(GC::Ref<PrimitiveString> value)
    {
        m_$[4] = value;
        m_parens_materialized = true;
    }
    void set_$6(GC::Ref<PrimitiveString> value)
    {
        m_$[5] = value;
        m_parens_materialized = true;
    }
    void set_$7(GC::Ref<PrimitiveString> value)
    {
        m_$[6] = value;
        m_parens_materialized = true;
    }
    void set_$8(GC::Ref<PrimitiveString> value)
    {
        m_$[7] = value;
        m_parens_materialized = true;
    }
    void set_$9(GC::Ref<PrimitiveString> value)
    {
        m_$[8] = value;
        m_parens_materialized = true;
    }

    // Lazy update: store capture indices without materializing strings.
    // Strings are created on demand when $1-$9 are accessed.
    void set_captures_lazy(size_t num_captures, int const* capture_starts, int const* capture_ends);

    void invalidate();
    void visit_edges(Cell::Visitor&);

private:
    GC::Ptr<PrimitiveString> lazy_paren(size_t index) const;
    GC::Ref<PrimitiveString> empty_string() const;
    GC::Ref<PrimitiveString> substring_of_match_source(size_t start, size_t length) const;

    GC::Ptr<PrimitiveString> m_input;
    GC::Ptr<PrimitiveString> m_match_source;
    mutable GC::Ptr<PrimitiveString> m_last_paren;
    mutable GC::Ptr<PrimitiveString> m_$[9];

    size_t m_last_match_start { 0 };
    size_t m_last_match_length { 0 };
    size_t m_left_context_start { 0 };
    size_t m_left_context_length { 0 };
    size_t m_right_context_start { 0 };
    size_t m_right_context_length { 0 };
    mutable GC::Ptr<PrimitiveString> m_last_match_string;
    mutable GC::Ptr<PrimitiveString> m_left_context_string;
    mutable GC::Ptr<PrimitiveString> m_right_context_string;
    int m_last_paren_start { -1 };
    int m_last_paren_end { -1 };

    // Lazy capture storage: indices into m_match_source.
    // -1 means not captured. Strings materialized on access.
    int m_paren_starts[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    int m_paren_ends[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    mutable bool m_parens_materialized = true;
};

ThrowCompletionOr<void> set_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, void (RegExpLegacyStaticProperties::*property_setter)(GC::Ref<PrimitiveString>), Value value);
ThrowCompletionOr<Value> get_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, GC::Ptr<PrimitiveString> (RegExpLegacyStaticProperties::*property_getter)() const);
void update_legacy_regexp_static_properties(RegExpConstructor& constructor, GC::Ref<PrimitiveString> string, size_t start_index, size_t end_index, Vector<GC::Ref<PrimitiveString>> const& captured_values);
void update_legacy_regexp_static_properties_lazy(RegExpConstructor& constructor, GC::Ref<PrimitiveString> string, size_t start_index, size_t end_index, size_t num_captures, int const* capture_starts, int const* capture_ends);
void invalidate_legacy_regexp_static_properties(RegExpConstructor& constructor);

}
