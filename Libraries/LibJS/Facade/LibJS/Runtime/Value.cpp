/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/StringBuilder.h>
#include <AK/StringConversions.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(to_underlying(GC::CellKind::Object) == JS_LAYOUT_CELL_KIND_OBJECT);
static_assert(to_underlying(GC::CellKind::PrimitiveString) == JS_LAYOUT_CELL_KIND_PRIMITIVE_STRING);
static_assert(to_underlying(GC::CellKind::Symbol) == JS_LAYOUT_CELL_KIND_SYMBOL);
static_assert(to_underlying(GC::CellKind::BigInt) == JS_LAYOUT_CELL_KIND_BIG_INT);
static_assert(to_underlying(GC::CellKind::Accessor) == JS_LAYOUT_CELL_KIND_ACCESSOR);

// Number::toString needs nothing of the runtime, so the facade formats numbers itself, exactly as the C++ runtime does.
static void append_ascii_for_number(StringBuilder& builder, char code_unit)
{
    builder.append(code_unit);
}

static void append_ascii_for_number(Utf16StringBuilder& builder, char code_unit)
{
    builder.append_ascii(code_unit);
}

static void append_ascii_for_number(StringBuilder& builder, StringView string)
{
    builder.append(string);
}

static void append_ascii_for_number(Utf16StringBuilder& builder, StringView string)
{
    builder.append_ascii(string);
}

static void append_ascii_for_number(StringBuilder& builder, char const* string, size_t length)
{
    builder.append(string, length);
}

static void append_ascii_for_number(Utf16StringBuilder& builder, char const* string, size_t length)
{
    builder.append_ascii(StringView { string, length });
}

static void append_repeated_ascii_for_number(StringBuilder& builder, char code_unit, size_t count)
{
    builder.append_repeated(code_unit, count);
}

static void append_repeated_ascii_for_number(Utf16StringBuilder& builder, char code_unit, size_t count)
{
    builder.append_repeated_ascii(code_unit, count);
}

// 6.1.6.1.20 Number::toString ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-tostring
// Implementation for radix = 10
template<typename Builder>
static void number_to_string_impl(Builder& builder, double d, NumberToStringMode mode)
{
    auto convert_to_decimal_digits_array = [](auto x, auto& digits, auto& length) {
        for (; x; x /= 10)
            digits[length++] = x % 10 | '0';
        for (i32 i = 0; 2 * i + 1 < length; ++i)
            swap(digits[i], digits[length - i - 1]);
    };

    // 1. If x is NaN, return "NaN".
    if (isnan(d)) {
        append_ascii_for_number(builder, "NaN"sv);
        return;
    }

    // 2. If x is +0𝔽 or -0𝔽, return "0".
    if (d == +0.0 || d == -0.0) {
        append_ascii_for_number(builder, "0"sv);
        return;
    }

    // 4. If x is +∞𝔽, return "Infinity".
    if (isinf(d)) {
        if (d > 0) {
            append_ascii_for_number(builder, "Infinity"sv);
            return;
        }

        append_ascii_for_number(builder, "-Infinity"sv);
        return;
    }

    // 5. Let n, k, and s be integers such that k ≥ 1, radix ^ (k - 1) ≤ s < radix ^ k, 𝔽(s × radix ^ (n - k)) is x, and
    //    k is as small as possible. Note that k is the number of digits in the representation of s using radix radix,
    //    that s is not divisible by radix, and that the least significant digit of s is not necessarily uniquely
    //    determined by these criteria.
    //
    // NB: guarantees provided by convert_to_decimal_exponential_form satisfy requirements of NOTE 2.
    auto [sign, mantissa, exponent] = AK::convert_to_decimal_exponential_form(d);
    i32 k = 0;
    AK::Array<char, 20> mantissa_digits;
    convert_to_decimal_digits_array(mantissa, mantissa_digits, k);

    i32 n = exponent + k; // s = mantissa

    // 3. If x < -0𝔽, return the string-concatenation of "-" and Number::toString(-x, radix).
    if (sign)
        append_ascii_for_number(builder, '-');

    // Non-standard: Intl needs number-to-string conversions for extremely large numbers without any
    // exponential formatting, as it will handle such formatting itself in a locale-aware way.
    bool force_no_exponent = mode == NumberToStringMode::WithoutExponent;

    // 6. If radix ≠ 10 or n is in the inclusive interval from -5 to 21, then
    if ((n >= -5 && n <= 21) || force_no_exponent) {
        // a. If n ≥ k, then
        if (n >= k) {
            // i. Return the string-concatenation of:
            // the code units of the k digits of the representation of s using radix radix
            append_ascii_for_number(builder, mantissa_digits.data(), k);
            // n - k occurrences of the code unit 0x0030 (DIGIT ZERO)
            append_repeated_ascii_for_number(builder, '0', n - k);
            // b. Else if n > 0, then
        } else if (n > 0) {
            // i. Return the string-concatenation of:
            // the code units of the most significant n digits of the representation of s using radix radix
            append_ascii_for_number(builder, mantissa_digits.data(), n);
            // the code unit 0x002E (FULL STOP)
            append_ascii_for_number(builder, '.');
            // the code units of the remaining k - n digits of the representation of s using radix radix
            append_ascii_for_number(builder, mantissa_digits.data() + n, k - n);
            // c. Else,
        } else {
            // i. Assert: n ≤ 0.
            VERIFY(n <= 0);
            // ii. Return the string-concatenation of:
            // the code unit 0x0030 (DIGIT ZERO)
            append_ascii_for_number(builder, '0');
            // the code unit 0x002E (FULL STOP)
            append_ascii_for_number(builder, '.');
            // -n occurrences of the code unit 0x0030 (DIGIT ZERO)
            append_repeated_ascii_for_number(builder, '0', -n);
            // the code units of the k digits of the representation of s using radix radix
            append_ascii_for_number(builder, mantissa_digits.data(), k);
        }

        return;
    }

    // 7. NOTE: In this case, the input will be represented using scientific E notation, such as 1.2e+3.

    // 9. If n < 0, then
    //     a. Let exponentSign be the code unit 0x002D (HYPHEN-MINUS).
    // 10. Else,
    //     a. Let exponentSign be the code unit 0x002B (PLUS SIGN).
    char exponent_sign = n < 0 ? '-' : '+';

    AK::Array<char, 5> exponent_digits;
    i32 exponent_length = 0;
    convert_to_decimal_digits_array(abs(n - 1), exponent_digits, exponent_length);

    // 11. If k is 1, then
    if (k == 1) {
        // a. Return the string-concatenation of:
        // the code unit of the single digit of s
        append_ascii_for_number(builder, mantissa_digits[0]);
        // the code unit 0x0065 (LATIN SMALL LETTER E)
        append_ascii_for_number(builder, 'e');
        // exponentSign
        append_ascii_for_number(builder, exponent_sign);
        // the code units of the decimal representation of abs(n - 1)
        append_ascii_for_number(builder, exponent_digits.data(), exponent_length);

        return;
    }

    // 12. Return the string-concatenation of:
    // the code unit of the most significant digit of the decimal representation of s
    append_ascii_for_number(builder, mantissa_digits[0]);
    // the code unit 0x002E (FULL STOP)
    append_ascii_for_number(builder, '.');
    // the code units of the remaining k - 1 digits of the decimal representation of s
    append_ascii_for_number(builder, mantissa_digits.data() + 1, k - 1);
    // the code unit 0x0065 (LATIN SMALL LETTER E)
    append_ascii_for_number(builder, 'e');
    // exponentSign
    append_ascii_for_number(builder, exponent_sign);
    // the code units of the decimal representation of abs(n - 1)
    append_ascii_for_number(builder, exponent_digits.data(), exponent_length);
}

void number_to_string(StringBuilder& builder, double d, NumberToStringMode mode)
{
    number_to_string_impl(builder, d, mode);
}

Utf16String number_to_utf16_string(double d, NumberToStringMode mode)
{
    Utf16StringBuilder builder;
    number_to_string_impl(builder, d, mode);
    return builder.to_string();
}

// 7.2.2 IsArray ( argument ), https://tc39.es/ecma262/#sec-isarray
ThrowCompletionOr<bool> Value::is_array(VM& vm) const
{
    return completion_from_abi<bool>(js_value_is_array(vm_to_abi(vm), value_to_abi(*this)));
}

// 7.2.3 IsCallable ( argument ), https://tc39.es/ecma262/#sec-iscallable
bool Value::is_function() const
{
    return is_object() && ::is<FunctionObject>(as_object());
}

// 7.2.4 IsConstructor ( argument ), https://tc39.es/ecma262/#sec-isconstructor
bool Value::is_constructor() const
{
    return js_value_is_constructor(value_to_abi(*this));
}

FunctionObject& Value::as_function()
{
    VERIFY(is_function());
    return static_cast<FunctionObject&>(as_object());
}

FunctionObject const& Value::as_function() const
{
    VERIFY(is_function());
    return static_cast<FunctionObject const&>(as_object());
}

// 7.1.17 ToString ( argument ), https://tc39.es/ecma262/#sec-tostring
ThrowCompletionOr<Utf16String> Value::to_utf16_string(VM& vm) const
{
    JSOwnedUtf16String string {};
    TRY(completion_from_abi<void>(js_value_to_utf16_string(vm_to_abi(vm), value_to_abi(*this), &string)));
    return owned_utf16_string_from_abi(string);
}

Utf16String Value::to_utf16_string_without_side_effects() const
{
    return owned_utf16_string_from_abi(js_value_to_utf16_string_without_side_effects(value_to_abi(*this)));
}

// 7.1.2 ToBoolean ( argument ), https://tc39.es/ecma262/#sec-toboolean
bool Value::to_boolean_slow_case() const
{
    return js_value_to_boolean(value_to_abi(*this));
}

// 7.1.18 ToObject ( argument ), https://tc39.es/ecma262/#sec-toobject
ThrowCompletionOr<GC::Ref<Object>> Value::to_object_slow(VM& vm) const
{
    return completion_from_abi<GC::Ref<Object>>(js_value_to_object(vm_to_abi(vm), value_to_abi(*this)));
}

// 7.1.4 ToNumber ( argument ), https://tc39.es/ecma262/#sec-tonumber
ThrowCompletionOr<Value> Value::to_number_slow_case(VM& vm) const
{
    return completion_from_abi<Value>(js_value_to_number(vm_to_abi(vm), value_to_abi(*this)));
}

// 7.1.13 ToBigInt ( argument ), https://tc39.es/ecma262/#sec-tobigint
ThrowCompletionOr<GC::Ref<BigInt>> Value::to_bigint(VM& vm) const
{
    return completion_from_abi<GC::Ref<BigInt>>(js_value_to_bigint(vm_to_abi(vm), value_to_abi(*this)));
}

// 7.1.16 ToBigUint64 ( argument ), https://tc39.es/ecma262/#sec-tobiguint64
ThrowCompletionOr<u64> Value::to_bigint_uint64(VM& vm) const
{
    u64 result = 0;
    TRY(completion_from_abi<void>(js_value_to_bigint_uint64(vm_to_abi(vm), value_to_abi(*this), &result)));
    return result;
}

ThrowCompletionOr<double> Value::to_double(VM& vm) const
{
    if (is_number())
        return as_double();
    double result = 0;
    TRY(completion_from_abi<void>(js_value_to_double(vm_to_abi(vm), value_to_abi(*this), &result)));
    return result;
}

// 7.1.6 ToInt32 ( argument ), https://tc39.es/ecma262/#sec-toint32
ThrowCompletionOr<i32> Value::to_i32(VM& vm) const
{
    if (is_int32())
        return as_i32();

#if __has_builtin(__builtin_arm_jcvt)
    if (is_double())
        return __builtin_arm_jcvt(m_value.as_double);
#endif

    i32 result = 0;
    TRY(completion_from_abi<void>(js_value_to_i32(vm_to_abi(vm), value_to_abi(*this), &result)));
    return result;
}

// 7.1.7 ToUint32 ( argument ), https://tc39.es/ecma262/#sec-touint32
ThrowCompletionOr<u32> Value::to_u32(VM& vm) const
{
    // OPTIMIZATION: ToUint32 is ToInt32 reinterpreted, so the fast paths of to_i32() apply.
    if (is_number())
        return static_cast<u32>(TRY(to_i32(vm)));

    u32 result = 0;
    TRY(completion_from_abi<void>(js_value_to_u32(vm_to_abi(vm), value_to_abi(*this), &result)));
    return result;
}

// 7.1.9 ToUint16 ( argument ), https://tc39.es/ecma262/#sec-touint16
ThrowCompletionOr<u16> Value::to_u16(VM& vm) const
{
    u16 result = 0;
    TRY(completion_from_abi<void>(js_value_to_u16(vm_to_abi(vm), value_to_abi(*this), &result)));
    return result;
}

// 7.1.11 ToUint8 ( argument ), https://tc39.es/ecma262/#sec-touint8
ThrowCompletionOr<u8> Value::to_u8(VM& vm) const
{
    // OPTIMIZATION: Fast path for the common case of an int32.
    if (is_int32())
        return static_cast<u8>(as_i32() & NumericLimits<u8>::max());

    u8 result = 0;
    TRY(completion_from_abi<void>(js_value_to_u8(vm_to_abi(vm), value_to_abi(*this), &result)));
    return result;
}

// 7.1.20 ToLength ( argument ), https://tc39.es/ecma262/#sec-tolength
ThrowCompletionOr<size_t> Value::to_length(VM& vm) const
{
    u64 result = 0;
    TRY(completion_from_abi<void>(js_value_to_length(vm_to_abi(vm), value_to_abi(*this), &result)));
    return result;
}

// 7.3.3 GetV ( V, P ), https://tc39.es/ecma262/#sec-getv
ThrowCompletionOr<Value> Value::get(VM& vm, PropertyKey const& property_key) const
{
    // 1. Let O be ? ToObject(V).
    auto object = TRY(to_object(vm));

    // 2. Return ? O.[[Get]](P, V).
    return completion_from_abi<Value>(js_object_internal_get(vm_to_abi(vm), object_to_abi(*object), property_key_to_abi(property_key), value_to_abi(*this), nullptr, JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY));
}

// 7.3.11 GetMethod ( V, P ), https://tc39.es/ecma262/#sec-getmethod
ThrowCompletionOr<GC::Ptr<FunctionObject>> Value::get_method(VM& vm, PropertyKey const& property_key) const
{
    return completion_from_abi<GC::Ptr<FunctionObject>>(js_function_get_method(vm_to_abi(vm), value_to_abi(*this), property_key_to_abi(property_key)));
}

// 7.2.10 SameValue ( x, y ), https://tc39.es/ecma262/#sec-samevalue
bool same_value(Value lhs, Value rhs)
{
    return js_value_same_value(value_to_abi(lhs), value_to_abi(rhs));
}

// 7.2.11 SameValueZero ( x, y ), https://tc39.es/ecma262/#sec-samevaluezero
bool same_value_zero(Value lhs, Value rhs)
{
    return js_value_same_value_zero(value_to_abi(lhs), value_to_abi(rhs));
}

}
