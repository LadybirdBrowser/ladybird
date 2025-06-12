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
    m_input = {};
    m_last_match = {};
    m_last_match_string = {};
    m_last_paren = {};
    m_left_context = {};
    m_left_context_string = {};
    m_right_context = {};
    m_right_context_string = {};
    m_$1 = {};
    m_$2 = {};
    m_$3 = {};
    m_$4 = {};
    m_$5 = {};
    m_$6 = {};
    m_$7 = {};
    m_$8 = {};
    m_$9 = {};
}

// GetLegacyRegExpStaticProperty( C, thisValue, internalSlotName ), https://github.com/tc39/proposal-regexp-legacy-features#getlegacyregexpstaticproperty-c-thisvalue-internalslotname-
ThrowCompletionOr<Value> get_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, Optional<Utf16String> const& (RegExpLegacyStaticProperties::*property_getter)() const)
{
    // 1. Assert C is an object that has an internal slot named internalSlotName.

    // 2. If SameValue(C, thisValue) is false, throw a TypeError exception.
    if (!same_value(&constructor, this_value))
        return vm.throw_completion<TypeError>(ErrorType::GetLegacyRegExpStaticPropertyThisValueMismatch);

    // 3. Let val be the value of the internal slot of C named internalSlotName.
    auto val = (constructor.legacy_static_properties().*property_getter)();

    // 4. If val is empty, throw a TypeError exception.
    if (!val.has_value())
        return vm.throw_completion<TypeError>(ErrorType::GetLegacyRegExpStaticPropertyValueEmpty);

    // 5. Return val.
    return PrimitiveString::create(vm, val.release_value());
}

// SetLegacyRegExpStaticProperty( C, thisValue, internalSlotName, val ), https://github.com/tc39/proposal-regexp-legacy-features#setlegacyregexpstaticproperty-c-thisvalue-internalslotname-val-
ThrowCompletionOr<void> set_legacy_regexp_static_property(VM& vm, RegExpConstructor& constructor, Value this_value, void (RegExpLegacyStaticProperties::*property_setter)(Utf16String), Value value)
{
    // 1. Assert C is an object that has an internal slot named internalSlotName.

    // 2. If SameValue(C, thisValue) is false, throw a TypeError exception.
    if (!same_value(&constructor, this_value))
        return vm.throw_completion<TypeError>(ErrorType::SetLegacyRegExpStaticPropertyThisValueMismatch);

    // 3. Let strVal be ? ToString(val).
    auto str_value = TRY(value.to_utf16_string(vm));

    // 4. Set the value of the internal slot of C named internalSlotName to strVal.
    (constructor.legacy_static_properties().*property_setter)(str_value);

    return {};
}

// UpdateLegacyRegExpStaticProperties ( C, S, startIndex, endIndex, capturedValues ), https://github.com/tc39/proposal-regexp-legacy-features#updatelegacyregexpstaticproperties--c-s-startindex-endindex-capturedvalues-
void update_legacy_regexp_static_properties(RegExpConstructor& constructor, Utf16String const& string, size_t start_index, size_t end_index, Vector<Utf16String> const& captured_values)
{
    auto& legacy_static_properties = constructor.legacy_static_properties();

    // 1. Assert: C is an Object that has a [[RegExpInput]] internal slot.
    // 2. Assert: Type(S) is String.

    // 3. Let len be the number of code units in S.
    auto len = string.length_in_code_units();

    // 4. Assert: startIndex and endIndex are integers such that 0 ≤ startIndex ≤ endIndex ≤ len.
    VERIFY(start_index <= end_index);
    VERIFY(end_index <= len);

    // 5. Assert: capturedValues is a List of Strings.

    // 6. Let n be the number of elements in capturedValues.
    auto group_count = captured_values.size();

    // 7. Set the value of C’s [[RegExpInput]] internal slot to S.
    legacy_static_properties.set_input(string);

    // 8. Set the value of C’s [[RegExpLastMatch]] internal slot to a String whose length is endIndex - startIndex and containing the code units from S with indices startIndex through endIndex - 1, in ascending order.
    auto last_match = string.view().substring_view(start_index, end_index - start_index);
    legacy_static_properties.set_last_match(last_match);

    // 9. If n > 0, set the value of C’s [[RegExpLastParen]] internal slot to the last element of capturedValues.
    if (group_count > 0) {
        auto item = captured_values[group_count - 1];
        legacy_static_properties.set_last_paren(item);
    }
    // 10. Else, set the value of C’s [[RegExpLastParen]] internal slot to the empty String.
    else {
        legacy_static_properties.set_last_paren(Utf16String::create());
    }

    // 11. Set the value of C’s [[RegExpLeftContext]] internal slot to a String whose length is startIndex and containing the code units from S with indices 0 through startIndex - 1, in ascending order.
    auto left_context = string.view().substring_view(0, start_index);
    legacy_static_properties.set_left_context(left_context);

    // 12. Set the value of C’s [[RegExpRightContext]] internal slot to a String whose length is len - endIndex and containing the code units from S with indices endIndex through len - 1, in ascending order.
    auto right_context = string.view().substring_view(end_index, len - end_index);
    legacy_static_properties.set_right_context(right_context);

    // 13. For each integer i such that 1 ≤ i ≤ 9
    for (size_t i = 1; i <= 9; i++) {
        // i. If i ≤ n, set the value of C’s [[RegExpPareni]] internal slot to the ith element of capturedValues.
        // ii. Else, set the value of C’s [[RegExpPareni]] internal slot to the empty String.
        auto value = (i <= group_count) ? captured_values[i - 1] : Utf16String::create();

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

// InvalidateLegacyRegExpStaticProperties ( C ), https://github.com/tc39/proposal-regexp-legacy-features#invalidatelegacyregexpstaticproperties--c
void invalidate_legacy_regexp_static_properties(RegExpConstructor& constructor)
{
    // 1. Assert: C is an Object that has a [[RegExpInput]] internal slot.

    // 2. Set the value of the following internal slots of C to empty:
    constructor.legacy_static_properties().invalidate();
}

}
