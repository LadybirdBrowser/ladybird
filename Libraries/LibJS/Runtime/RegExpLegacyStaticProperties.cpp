/*
 * Copyright (c) 2022, LI YUBEI <leeight@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibJS/Runtime/RegExpConstructor.h>
#include <LibJS/Runtime/RegExpLegacyStaticProperties.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

void RegExpLegacyStaticProperties::invalidate()
{
    m_input = nullptr;
    m_match_source = nullptr;
    m_last_match_start = 0;
    m_last_match_length = 0;
    m_last_match_string = nullptr;
    m_last_paren = nullptr;
    m_last_paren_start = -1;
    m_last_paren_end = -1;
    m_left_context_start = 0;
    m_left_context_length = 0;
    m_left_context_string = nullptr;
    m_right_context_start = 0;
    m_right_context_length = 0;
    m_right_context_string = nullptr;
    for (auto& p : m_$)
        p = nullptr;
    for (auto& s : m_paren_starts)
        s = -1;
    for (auto& e : m_paren_ends)
        e = -1;
    m_parens_materialized = true;
}

void RegExpLegacyStaticProperties::visit_edges(Cell::Visitor& visitor)
{
    visitor.visit(m_input);
    visitor.visit(m_match_source);
    visitor.visit(m_last_paren);
    for (auto& paren : m_$)
        visitor.visit(paren);
    visitor.visit(m_last_match_string);
    visitor.visit(m_left_context_string);
    visitor.visit(m_right_context_string);
}

void RegExpLegacyStaticProperties::set_match_source(GC::Ref<PrimitiveString> match_source)
{
    m_match_source = match_source;
    m_last_match_string = nullptr;
    m_last_paren = nullptr;
    m_left_context_string = nullptr;
    m_right_context_string = nullptr;
    for (auto& paren : m_$)
        paren = nullptr;
}

void RegExpLegacyStaticProperties::set_last_match(size_t start, size_t length)
{
    m_last_match_start = start;
    m_last_match_length = length;
    m_last_match_string = nullptr;
}

void RegExpLegacyStaticProperties::set_last_paren(GC::Ref<PrimitiveString> last_paren)
{
    m_last_paren = last_paren;
    m_last_paren_start = -1;
    m_last_paren_end = -1;
}

void RegExpLegacyStaticProperties::set_left_context(size_t start, size_t length)
{
    m_left_context_start = start;
    m_left_context_length = length;
    m_left_context_string = nullptr;
}

void RegExpLegacyStaticProperties::set_right_context(size_t start, size_t length)
{
    m_right_context_start = start;
    m_right_context_length = length;
    m_right_context_string = nullptr;
}

GC::Ref<PrimitiveString> RegExpLegacyStaticProperties::empty_string() const
{
    VERIFY(m_match_source);
    return m_match_source->vm().empty_string();
}

GC::Ref<PrimitiveString> RegExpLegacyStaticProperties::substring_of_match_source(size_t start, size_t length) const
{
    VERIFY(m_match_source);
    return PrimitiveString::create(m_match_source->vm(), *m_match_source, start, length);
}

GC::Ptr<PrimitiveString> RegExpLegacyStaticProperties::last_match() const
{
    if (!m_match_source)
        return nullptr;
    if (!m_last_match_string)
        m_last_match_string = substring_of_match_source(m_last_match_start, m_last_match_length);
    return m_last_match_string;
}

GC::Ptr<PrimitiveString> RegExpLegacyStaticProperties::last_paren() const
{
    if (!m_match_source)
        return nullptr;
    if (!m_last_paren) {
        if (m_last_paren_start >= 0 && m_last_paren_end >= 0)
            m_last_paren = substring_of_match_source(m_last_paren_start, m_last_paren_end - m_last_paren_start);
        else
            m_last_paren = empty_string();
    }
    return m_last_paren;
}

GC::Ptr<PrimitiveString> RegExpLegacyStaticProperties::left_context() const
{
    if (!m_match_source)
        return nullptr;
    if (!m_left_context_string)
        m_left_context_string = substring_of_match_source(m_left_context_start, m_left_context_length);
    return m_left_context_string;
}

GC::Ptr<PrimitiveString> RegExpLegacyStaticProperties::right_context() const
{
    if (!m_match_source)
        return nullptr;
    if (!m_right_context_string)
        m_right_context_string = substring_of_match_source(m_right_context_start, m_right_context_length);
    return m_right_context_string;
}

GC::Ptr<PrimitiveString> RegExpLegacyStaticProperties::lazy_paren(size_t index) const
{
    VERIFY(index < 9);
    if (!m_match_source)
        return nullptr;
    if (!m_parens_materialized) {
        // Materialize all lazy parens from stored indices.
        for (size_t i = 0; i < 9; i++) {
            if (m_paren_starts[i] >= 0 && m_paren_ends[i] >= 0)
                m_$[i] = substring_of_match_source(m_paren_starts[i], m_paren_ends[i] - m_paren_starts[i]);
            else
                m_$[i] = empty_string();
        }
        m_parens_materialized = true;
    }
    return m_$[index];
}

void RegExpLegacyStaticProperties::set_captures_lazy(size_t num_captures, int const* capture_starts, int const* capture_ends)
{
    for (size_t i = 0; i < 9; i++) {
        if (i < num_captures) {
            m_paren_starts[i] = capture_starts[i];
            m_paren_ends[i] = capture_ends[i];
        } else {
            m_paren_starts[i] = -1;
            m_paren_ends[i] = -1;
        }
    }
    // Clear any previously materialized strings.
    for (auto& p : m_$)
        p = nullptr;
    m_parens_materialized = false;

    // Set last_paren to the last captured value.
    if (num_captures > 0 && capture_starts[num_captures - 1] >= 0 && capture_ends[num_captures - 1] >= 0) {
        m_last_paren = nullptr;
        m_last_paren_start = capture_starts[num_captures - 1];
        m_last_paren_end = capture_ends[num_captures - 1];
    } else {
        m_last_paren = empty_string();
        m_last_paren_start = -1;
        m_last_paren_end = -1;
    }
}

// GetLegacyRegExpStaticProperty( C, thisValue, internalSlotName ), https://github.com/tc39/proposal-regexp-legacy-features#getlegacyregexpstaticproperty-c-thisvalue-internalslotname-
ThrowCompletionOr<Value> get_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, GC::Ptr<PrimitiveString> (RegExpLegacyStaticProperties::*property_getter)() const)
{
    // 1. Assert C is an object that has an internal slot named internalSlotName.

    // 2. If SameValue(C, thisValue) is false, throw a TypeError exception.
    if (!same_value(&constructor, this_value))
        return vm.throw_completion<TypeError>(ErrorType::GetLegacyRegExpStaticPropertyThisValueMismatch);

    // 3. Let val be the value of the internal slot of C named internalSlotName.
    auto val = (constructor.legacy_static_properties().*property_getter)();

    // 4. If val is empty, throw a TypeError exception.
    if (!val)
        return vm.throw_completion<TypeError>(ErrorType::GetLegacyRegExpStaticPropertyValueEmpty);

    // 5. Return val.
    return val;
}

// SetLegacyRegExpStaticProperty( C, thisValue, internalSlotName, val ), https://github.com/tc39/proposal-regexp-legacy-features#setlegacyregexpstaticproperty-c-thisvalue-internalslotname-val-
ThrowCompletionOr<void> set_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, void (RegExpLegacyStaticProperties::*property_setter)(GC::Ref<PrimitiveString>), Value value)
{
    // 1. Assert C is an object that has an internal slot named internalSlotName.

    // 2. If SameValue(C, thisValue) is false, throw a TypeError exception.
    if (!same_value(&constructor, this_value))
        return vm.throw_completion<TypeError>(ErrorType::SetLegacyRegExpStaticPropertyThisValueMismatch);

    // 3. Let strVal be ? ToString(val).
    auto str_value = TRY(value.to_primitive_string(vm));

    // 4. Set the value of the internal slot of C named internalSlotName to strVal.
    (constructor.legacy_static_properties().*property_setter)(str_value);

    return {};
}

// UpdateLegacyRegExpStaticProperties ( C, S, startIndex, endIndex, capturedValues ), https://github.com/tc39/proposal-regexp-legacy-features#updatelegacyregexpstaticproperties--c-s-startindex-endindex-capturedvalues-
void update_legacy_regexp_static_properties(RegExpConstructor& constructor, GC::Ref<PrimitiveString> string, size_t start_index, size_t end_index, Vector<GC::Ref<PrimitiveString>> const& captured_values)
{
    auto& legacy_static_properties = constructor.legacy_static_properties();

    // 1. Assert: C is an Object that has a [[RegExpInput]] internal slot.
    // 2. Assert: Type(S) is String.

    // 3. Let len be the number of code units in S.
    auto len = string->length_in_utf16_code_units();

    // 4. Assert: startIndex and endIndex are integers such that 0 ≤ startIndex ≤ endIndex ≤ len.
    VERIFY(start_index <= end_index);
    VERIFY(end_index <= len);

    // 5. Assert: capturedValues is a List of Strings.

    // 6. Let n be the number of elements in capturedValues.
    auto group_count = captured_values.size();

    // 7. Set the value of C’s [[RegExpInput]] internal slot to S.
    legacy_static_properties.set_input(string);
    legacy_static_properties.set_match_source(string);

    // 8. Set the value of C’s [[RegExpLastMatch]] internal slot to a String whose length is endIndex - startIndex and containing the code units from S with indices startIndex through endIndex - 1, in ascending order.
    legacy_static_properties.set_last_match(start_index, end_index - start_index);

    // 9. If n > 0, set the value of C’s [[RegExpLastParen]] internal slot to the last element of capturedValues.
    if (group_count > 0) {
        auto item = captured_values[group_count - 1];
        legacy_static_properties.set_last_paren(item);
    }
    // 10. Else, set the value of C’s [[RegExpLastParen]] internal slot to the empty String.
    else {
        legacy_static_properties.set_last_paren(string->vm().empty_string());
    }

    // 11. Set the value of C’s [[RegExpLeftContext]] internal slot to a String whose length is startIndex and containing the code units from S with indices 0 through startIndex - 1, in ascending order.
    legacy_static_properties.set_left_context(0, start_index);

    // 12. Set the value of C’s [[RegExpRightContext]] internal slot to a String whose length is len - endIndex and containing the code units from S with indices endIndex through len - 1, in ascending order.
    legacy_static_properties.set_right_context(end_index, len - end_index);

    // 13. For each integer i such that 1 ≤ i ≤ 9
    for (size_t i = 1; i <= 9; i++) {
        // i. If i ≤ n, set the value of C’s [[RegExpPareni]] internal slot to the ith element of capturedValues.
        // ii. Else, set the value of C’s [[RegExpPareni]] internal slot to the empty String.
        GC::Ref<PrimitiveString> value = string->vm().empty_string();
        if (i <= group_count)
            value = captured_values[i - 1];

        if (i == 1) {
            legacy_static_properties.set_$1(move(value));
        } else if (i == 2) {
            legacy_static_properties.set_$2(move(value));
        } else if (i == 3) {
            legacy_static_properties.set_$3(move(value));
        } else if (i == 4) {
            legacy_static_properties.set_$4(move(value));
        } else if (i == 5) {
            legacy_static_properties.set_$5(move(value));
        } else if (i == 6) {
            legacy_static_properties.set_$6(move(value));
        } else if (i == 7) {
            legacy_static_properties.set_$7(move(value));
        } else if (i == 8) {
            legacy_static_properties.set_$8(move(value));
        } else if (i == 9) {
            legacy_static_properties.set_$9(move(value));
        }
    }
}

// Like update_legacy_regexp_static_properties, but defers $1-$9 string creation.
// Captures are stored as index pairs into the input string and materialized on access.
void update_legacy_regexp_static_properties_lazy(RegExpConstructor& constructor, GC::Ref<PrimitiveString> string, size_t start_index, size_t end_index, size_t num_captures, int const* capture_starts, int const* capture_ends)
{
    auto& legacy_static_properties = constructor.legacy_static_properties();

    auto len = string->length_in_utf16_code_units();
    VERIFY(start_index <= end_index);
    VERIFY(end_index <= len);

    legacy_static_properties.set_input(string);
    legacy_static_properties.set_match_source(string);

    legacy_static_properties.set_last_match(start_index, end_index - start_index);

    legacy_static_properties.set_left_context(0, start_index);

    legacy_static_properties.set_right_context(end_index, len - end_index);

    legacy_static_properties.set_captures_lazy(num_captures, capture_starts, capture_ends);
}

// InvalidateLegacyRegExpStaticProperties ( C ), https://github.com/tc39/proposal-regexp-legacy-features#invalidatelegacyregexpstaticproperties--c
void invalidate_legacy_regexp_static_properties(RegExpConstructor& constructor)
{
    // 1. Assert: C is an Object that has a [[RegExpInput]] internal slot.

    // 2. Set the value of the following internal slots of C to empty:
    constructor.legacy_static_properties().invalidate();
}

}
