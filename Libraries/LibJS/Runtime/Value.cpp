/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/FloatingPointStringConversions.h>
#include <AK/StringBuilder.h>
#include <AK/StringFloatingPointConversions.h>
#include <AK/Utf8View.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/NumberTheory/ModularFunctions.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/BoundFunction.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/ProxyObject.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/Runtime/SymbolObject.h>
#include <LibJS/Runtime/Utf16String.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

namespace JS {

static inline bool same_type_for_equality(Value const& lhs, Value const& rhs)
{
    // If the top two bytes are identical then either:
    // both are NaN boxed Values with the same type
    // or they are doubles which happen to have the same top bytes.
    if ((lhs.encoded() & GC::TAG_EXTRACTION) == (rhs.encoded() & GC::TAG_EXTRACTION))
        return true;

    if (lhs.is_number() && rhs.is_number())
        return true;

    // One of the Values is not a number and they do not have the same tag
    return false;
}

static Crypto::SignedBigInteger const BIGINT_ZERO { 0 };

ALWAYS_INLINE bool both_number(Value const& lhs, Value const& rhs)
{
    return lhs.is_number() && rhs.is_number();
}

ALWAYS_INLINE bool both_bigint(Value const& lhs, Value const& rhs)
{
    return lhs.is_bigint() && rhs.is_bigint();
}

// 6.1.6.1.20 Number::toString ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-tostring
// Implementation for radix = 10
static void number_to_string_impl(StringBuilder& builder, double d, NumberToStringMode mode)
{
    auto convert_to_decimal_digits_array = [](auto x, auto& digits, auto& length) {
        for (; x; x /= 10)
            digits[length++] = x % 10 | '0';
        for (i32 i = 0; 2 * i + 1 < length; ++i)
            swap(digits[i], digits[length - i - 1]);
    };

    // 1. If x is NaN, return "NaN".
    if (isnan(d)) {
        builder.append("NaN"sv);
        return;
    }

    // 2. If x is +0𝔽 or -0𝔽, return "0".
    if (d == +0.0 || d == -0.0) {
        builder.append("0"sv);
        return;
    }

    // 4. If x is +∞𝔽, return "Infinity".
    if (isinf(d)) {
        if (d > 0) {
            builder.append("Infinity"sv);
            return;
        }

        builder.append("-Infinity"sv);
        return;
    }

    // 5. Let n, k, and s be integers such that k ≥ 1, radix ^ (k - 1) ≤ s < radix ^ k,
    // 𝔽(s × radix ^ (n - k)) is x, and k is as small as possible. Note that k is the number of
    // digits in the representation of s using radix radix, that s is not divisible by radix, and
    // that the least significant digit of s is not necessarily uniquely determined by these criteria.
    //
    // Note: guarantees provided by convert_floating_point_to_decimal_exponential_form satisfy
    //       requirements of NOTE 2.
    auto [sign, mantissa, exponent] = convert_floating_point_to_decimal_exponential_form(d);
    i32 k = 0;
    AK::Array<char, 20> mantissa_digits;
    convert_to_decimal_digits_array(mantissa, mantissa_digits, k);

    i32 n = exponent + k; // s = mantissa

    // 3. If x < -0𝔽, return the string-concatenation of "-" and Number::toString(-x, radix).
    if (sign)
        builder.append('-');

    // Non-standard: Intl needs number-to-string conversions for extremely large numbers without any
    // exponential formatting, as it will handle such formatting itself in a locale-aware way.
    bool force_no_exponent = mode == NumberToStringMode::WithoutExponent;

    // 6. If radix ≠ 10 or n is in the inclusive interval from -5 to 21, then
    if ((n >= -5 && n <= 21) || force_no_exponent) {
        // a. If n ≥ k, then
        if (n >= k) {
            // i. Return the string-concatenation of:
            // the code units of the k digits of the representation of s using radix radix
            builder.append(mantissa_digits.data(), k);
            // n - k occurrences of the code unit 0x0030 (DIGIT ZERO)
            builder.append_repeated('0', n - k);
            // b. Else if n > 0, then
        } else if (n > 0) {
            // i. Return the string-concatenation of:
            // the code units of the most significant n digits of the representation of s using radix radix
            builder.append(mantissa_digits.data(), n);
            // the code unit 0x002E (FULL STOP)
            builder.append('.');
            // the code units of the remaining k - n digits of the representation of s using radix radix
            builder.append(mantissa_digits.data() + n, k - n);
            // c. Else,
        } else {
            // i. Assert: n ≤ 0.
            VERIFY(n <= 0);
            // ii. Return the string-concatenation of:
            // the code unit 0x0030 (DIGIT ZERO)
            builder.append('0');
            // the code unit 0x002E (FULL STOP)
            builder.append('.');
            // -n occurrences of the code unit 0x0030 (DIGIT ZERO)
            builder.append_repeated('0', -n);
            // the code units of the k digits of the representation of s using radix radix
            builder.append(mantissa_digits.data(), k);
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
        builder.append(mantissa_digits[0]);
        // the code unit 0x0065 (LATIN SMALL LETTER E)
        builder.append('e');
        // exponentSign
        builder.append(exponent_sign);
        // the code units of the decimal representation of abs(n - 1)
        builder.append(exponent_digits.data(), exponent_length);

        return;
    }

    // 12. Return the string-concatenation of:
    // the code unit of the most significant digit of the decimal representation of s
    builder.append(mantissa_digits[0]);
    // the code unit 0x002E (FULL STOP)
    builder.append('.');
    // the code units of the remaining k - 1 digits of the decimal representation of s
    builder.append(mantissa_digits.data() + 1, k - 1);
    // the code unit 0x0065 (LATIN SMALL LETTER E)
    builder.append('e');
    // exponentSign
    builder.append(exponent_sign);
    // the code units of the decimal representation of abs(n - 1)
    builder.append(exponent_digits.data(), exponent_length);
}

String number_to_string(double d, NumberToStringMode mode)
{
    StringBuilder builder;
    number_to_string_impl(builder, d, mode);
    return builder.to_string().release_value();
}

ByteString number_to_byte_string(double d, NumberToStringMode mode)
{
    StringBuilder builder;
    number_to_string_impl(builder, d, mode);
    return builder.to_byte_string();
}

// 7.2.2 IsArray ( argument ), https://tc39.es/ecma262/#sec-isarray
ThrowCompletionOr<bool> Value::is_array(VM& vm) const
{
    // 1. If argument is not an Object, return false.
    if (!is_object())
        return false;

    auto const& object = as_object();

    // 2. If argument is an Array exotic object, return true.
    if (is<Array>(object))
        return true;

    // 3. If argument is a Proxy exotic object, then
    if (is<ProxyObject>(object)) {
        auto const& proxy = static_cast<ProxyObject const&>(object);

        // a. If argument.[[ProxyHandler]] is null, throw a TypeError exception.
        if (proxy.is_revoked())
            return vm.throw_completion<TypeError>(ErrorType::ProxyRevoked);

        // b. Let target be argument.[[ProxyTarget]].
        auto const& target = proxy.target();

        // c. Return ? IsArray(target).
        return Value(&target).is_array(vm);
    }

    // 4. Return false.
    return false;
}

Array& Value::as_array()
{
    VERIFY(is_object() && is<Array>(as_object()));
    return static_cast<Array&>(as_object());
}

// 20.5.8.2 IsError ( argument ), https://tc39.es/proposal-is-error/#sec-iserror
bool Value::is_error() const
{
    // 1. If argument is not an Object, return false.
    // 2. If argument has an [[ErrorData]] internal slot, return true.
    // 3. Return false.
    return is_object() && is<Error>(as_object());
}

// 7.2.3 IsCallable ( argument ), https://tc39.es/ecma262/#sec-iscallable
bool Value::is_function() const
{
    // 1. If argument is not an Object, return false.
    // 2. If argument has a [[Call]] internal method, return true.
    // 3. Return false.
    return is_object() && as_object().is_function();
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

// 7.2.4 IsConstructor ( argument ), https://tc39.es/ecma262/#sec-isconstructor
bool Value::is_constructor() const
{
    // 1. If Type(argument) is not Object, return false.
    if (!is_function())
        return false;

    // 2. If argument has a [[Construct]] internal method, return true.
    if (as_function().has_constructor())
        return true;

    // 3. Return false.
    return false;
}

// 7.2.8 IsRegExp ( argument ), https://tc39.es/ecma262/#sec-isregexp
ThrowCompletionOr<bool> Value::is_regexp(VM& vm) const
{
    // 1. If argument is not an Object, return false.
    if (!is_object())
        return false;

    // 2. Let matcher be ? Get(argument, @@match).
    auto matcher = TRY(as_object().get(vm.well_known_symbol_match()));

    // 3. If matcher is not undefined, return ToBoolean(matcher).
    if (!matcher.is_undefined())
        return matcher.to_boolean();

    // 4. If argument has a [[RegExpMatcher]] internal slot, return true.
    // 5. Return false.
    return is<RegExpObject>(as_object());
}

// 13.5.3 The typeof Operator, https://tc39.es/ecma262/#sec-typeof-operator
GC::Ref<PrimitiveString> Value::typeof_(VM& vm) const
{
    // 9. If val is a Number, return "number".
    if (is_number())
        return *vm.typeof_strings.number;

    switch (m_value.tag) {
    // 4. If val is undefined, return "undefined".
    case UNDEFINED_TAG:
        return *vm.typeof_strings.undefined;
    // 5. If val is null, return "object".
    case NULL_TAG:
        return *vm.typeof_strings.object;
    // 6. If val is a String, return "string".
    case STRING_TAG:
        return *vm.typeof_strings.string;
    // 7. If val is a Symbol, return "symbol".
    case SYMBOL_TAG:
        return *vm.typeof_strings.symbol;
    // 8. If val is a Boolean, return "boolean".
    case BOOLEAN_TAG:
        return *vm.typeof_strings.boolean;
    // 10. If val is a BigInt, return "bigint".
    case BIGINT_TAG:
        return *vm.typeof_strings.bigint;
    // 11. Assert: val is an Object.
    case OBJECT_TAG:
        // B.3.6.3 Changes to the typeof Operator, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot-typeof
        // 12. If val has an [[IsHTMLDDA]] internal slot, return "undefined".
        if (as_object().is_htmldda())
            return *vm.typeof_strings.undefined;
        // 13. If val has a [[Call]] internal slot, return "function".
        if (is_function())
            return *vm.typeof_strings.function;
        // 14. Return "object".
        return *vm.typeof_strings.object;
    default:
        VERIFY_NOT_REACHED();
    }
}

String Value::to_string_without_side_effects() const
{
    if (is_double())
        return number_to_string(m_value.as_double);

    switch (m_value.tag) {
    case UNDEFINED_TAG:
        return "undefined"_string;
    case NULL_TAG:
        return "null"_string;
    case BOOLEAN_TAG:
        return as_bool() ? "true"_string : "false"_string;
    case INT32_TAG:
        return String::number(as_i32());
    case STRING_TAG:
        return as_string().utf8_string();
    case SYMBOL_TAG:
        return as_symbol().descriptive_string().release_value();
    case BIGINT_TAG:
        return as_bigint().to_string().release_value();
    case OBJECT_TAG:
        return String::formatted("[object {}]", as_object().class_name()).release_value();
    case ACCESSOR_TAG:
        return "<accessor>"_string;
    case EMPTY_TAG:
        return "<empty>"_string;
    default:
        VERIFY_NOT_REACHED();
    }
}

ThrowCompletionOr<GC::Ref<PrimitiveString>> Value::to_primitive_string(VM& vm)
{
    if (is_string())
        return as_string();
    auto string = TRY(to_string(vm));
    return PrimitiveString::create(vm, move(string));
}

// 7.1.17 ToString ( argument ), https://tc39.es/ecma262/#sec-tostring
ThrowCompletionOr<String> Value::to_string(VM& vm) const
{
    if (is_double())
        return number_to_string(m_value.as_double);

    switch (m_value.tag) {
    // 1. If argument is a String, return argument.
    case STRING_TAG:
        return as_string().utf8_string();
    // 2. If argument is a Symbol, throw a TypeError exception.
    case SYMBOL_TAG:
        return vm.throw_completion<TypeError>(ErrorType::Convert, "symbol", "string");
    // 3. If argument is undefined, return "undefined".
    case UNDEFINED_TAG:
        return "undefined"_string;
    // 4. If argument is null, return "null".
    case NULL_TAG:
        return "null"_string;
    // 5. If argument is true, return "true".
    // 6. If argument is false, return "false".
    case BOOLEAN_TAG:
        return as_bool() ? "true"_string : "false"_string;
    // 7. If argument is a Number, return Number::toString(argument, 10).
    case INT32_TAG:
        return String::number(as_i32());
    // 8. If argument is a BigInt, return BigInt::toString(argument, 10).
    case BIGINT_TAG:
        return TRY_OR_THROW_OOM(vm, as_bigint().big_integer().to_base(10));
    // 9. Assert: argument is an Object.
    case OBJECT_TAG: {
        // 10. Let primValue be ? ToPrimitive(argument, string).
        auto primitive_value = TRY(to_primitive(vm, PreferredType::String));

        // 11. Assert: primValue is not an Object.
        VERIFY(!primitive_value.is_object());

        // 12. Return ? ToString(primValue).
        return primitive_value.to_string(vm);
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

// 7.1.17 ToString ( argument ), https://tc39.es/ecma262/#sec-tostring
ThrowCompletionOr<ByteString> Value::to_byte_string(VM& vm) const
{
    return TRY(to_string(vm)).to_byte_string();
}

ThrowCompletionOr<Utf16String> Value::to_utf16_string(VM& vm) const
{
    if (is_string())
        return as_string().utf16_string();

    auto utf8_string = TRY(to_string(vm));
    return Utf16String::create(utf8_string.bytes_as_string_view());
}

ThrowCompletionOr<String> Value::to_well_formed_string(VM& vm) const
{
    return ::JS::to_well_formed_string(TRY(to_utf16_string(vm)));
}

// 7.1.2 ToBoolean ( argument ), https://tc39.es/ecma262/#sec-toboolean
bool Value::to_boolean_slow_case() const
{
    if (is_double()) {
        if (is_nan())
            return false;
        return m_value.as_double != 0;
    }

    switch (m_value.tag) {
    // 1. If argument is a Boolean, return argument.
    case BOOLEAN_TAG:
        return as_bool();
    // 2. If argument is any of undefined, null, +0𝔽, -0𝔽, NaN, 0ℤ, or the empty String, return false.
    case UNDEFINED_TAG:
    case NULL_TAG:
        return false;
    case INT32_TAG:
        return as_i32() != 0;
    case STRING_TAG:
        return !as_string().is_empty();
    case BIGINT_TAG:
        return as_bigint().big_integer() != BIGINT_ZERO;
    case OBJECT_TAG:
        // B.3.6.1 Changes to ToBoolean, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot-to-boolean
        // 3. If argument is an Object and argument has an [[IsHTMLDDA]] internal slot, return false.
        if (as_object().is_htmldda())
            return false;
        // 4. Return true.
        return true;
    case SYMBOL_TAG:
        return true;
    default:
        VERIFY_NOT_REACHED();
    }
}

// 7.1.1 ToPrimitive ( input [ , preferredType ] ), https://tc39.es/ecma262/#sec-toprimitive
ThrowCompletionOr<Value> Value::to_primitive_slow_case(VM& vm, PreferredType preferred_type) const
{
    // 1. If input is an Object, then
    if (is_object()) {
        // a. Let exoticToPrim be ? GetMethod(input, @@toPrimitive).
        auto exotic_to_primitive = TRY(get_method(vm, vm.well_known_symbol_to_primitive()));

        // b. If exoticToPrim is not undefined, then
        if (exotic_to_primitive) {
            auto hint = [&]() -> ByteString {
                switch (preferred_type) {
                // i. If preferredType is not present, let hint be "default".
                case PreferredType::Default:
                    return "default";
                // ii. Else if preferredType is string, let hint be "string".
                case PreferredType::String:
                    return "string";
                // iii. Else,
                // 1. Assert: preferredType is number.
                // 2. Let hint be "number".
                case PreferredType::Number:
                    return "number";
                default:
                    VERIFY_NOT_REACHED();
                }
            }();

            // iv. Let result be ? Call(exoticToPrim, input, « hint »).
            auto result = TRY(call(vm, *exotic_to_primitive, *this, PrimitiveString::create(vm, hint)));

            // v. If result is not an Object, return result.
            if (!result.is_object())
                return result;

            // vi. Throw a TypeError exception.
            return vm.throw_completion<TypeError>(ErrorType::ToPrimitiveReturnedObject, to_string_without_side_effects(), hint);
        }

        // c. If preferredType is not present, let preferredType be number.
        if (preferred_type == PreferredType::Default)
            preferred_type = PreferredType::Number;

        // d. Return ? OrdinaryToPrimitive(input, preferredType).
        return as_object().ordinary_to_primitive(preferred_type);
    }

    // 2. Return input.
    return *this;
}

// 7.1.18 ToObject ( argument ), https://tc39.es/ecma262/#sec-toobject
ThrowCompletionOr<GC::Ref<Object>> Value::to_object(VM& vm) const
{
    auto& realm = *vm.current_realm();
    VERIFY(!is_empty());

    // Number
    if (is_number()) {
        // Return a new Number object whose [[NumberData]] internal slot is set to argument. See 21.1 for a description of Number objects.
        return NumberObject::create(realm, as_double());
    }

    switch (m_value.tag) {
    // Undefined
    // Null
    case UNDEFINED_TAG:
    case NULL_TAG:
        // Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefined);
    // Boolean
    case BOOLEAN_TAG:
        // Return a new Boolean object whose [[BooleanData]] internal slot is set to argument. See 20.3 for a description of Boolean objects.
        return BooleanObject::create(realm, as_bool());
    // String
    case STRING_TAG:
        // Return a new String object whose [[StringData]] internal slot is set to argument. See 22.1 for a description of String objects.
        return StringObject::create(realm, const_cast<JS::PrimitiveString&>(as_string()), realm.intrinsics().string_prototype());
    // Symbol
    case SYMBOL_TAG:
        // Return a new Symbol object whose [[SymbolData]] internal slot is set to argument. See 20.4 for a description of Symbol objects.
        return SymbolObject::create(realm, const_cast<JS::Symbol&>(as_symbol()));
    // BigInt
    case BIGINT_TAG:
        // Return a new BigInt object whose [[BigIntData]] internal slot is set to argument. See 21.2 for a description of BigInt objects.
        return BigIntObject::create(realm, const_cast<JS::BigInt&>(as_bigint()));
    // Object
    case OBJECT_TAG:
        // Return argument.
        return const_cast<Object&>(as_object());
    default:
        VERIFY_NOT_REACHED();
    }
}

// 7.1.3 ToNumeric ( value ), https://tc39.es/ecma262/#sec-tonumeric
FLATTEN ThrowCompletionOr<Value> Value::to_numeric_slow_case(VM& vm) const
{
    // 1. Let primValue be ? ToPrimitive(value, number).
    auto primitive_value = TRY(to_primitive(vm, Value::PreferredType::Number));

    // 2. If primValue is a BigInt, return primValue.
    if (primitive_value.is_bigint())
        return primitive_value;

    // 3. Return ? ToNumber(primValue).
    return primitive_value.to_number(vm);
}

constexpr bool is_ascii_number(u32 code_point)
{
    return is_ascii_digit(code_point) || code_point == '.' || (code_point == 'e' || code_point == 'E') || code_point == '+' || code_point == '-';
}

struct NumberParseResult {
    StringView literal;
    u8 base;
};

static Optional<NumberParseResult> parse_number_text(StringView text)
{
    NumberParseResult result {};

    auto check_prefix = [&](auto lower_prefix, auto upper_prefix) {
        if (text.length() <= 2)
            return false;
        if (!text.starts_with(lower_prefix) && !text.starts_with(upper_prefix))
            return false;
        return true;
    };

    // https://tc39.es/ecma262/#sec-tonumber-applied-to-the-string-type
    if (check_prefix("0b"sv, "0B"sv)) {
        if (!all_of(text.substring_view(2), is_ascii_binary_digit))
            return {};

        result.literal = text.substring_view(2);
        result.base = 2;
    } else if (check_prefix("0o"sv, "0O"sv)) {
        if (!all_of(text.substring_view(2), is_ascii_octal_digit))
            return {};

        result.literal = text.substring_view(2);
        result.base = 8;
    } else if (check_prefix("0x"sv, "0X"sv)) {
        if (!all_of(text.substring_view(2), is_ascii_hex_digit))
            return {};

        result.literal = text.substring_view(2);
        result.base = 16;
    } else {
        if (!all_of(text, is_ascii_number))
            return {};

        result.literal = text;
        result.base = 10;
    }

    return result;
}

// 7.1.4.1.1 StringToNumber ( str ), https://tc39.es/ecma262/#sec-stringtonumber
double string_to_number(StringView string)
{
    // 1. Let text be StringToCodePoints(str).
    auto text = Utf8View(string).trim(whitespace_characters, AK::TrimMode::Both).as_string();

    // 2. Let literal be ParseText(text, StringNumericLiteral).
    if (text.is_empty())
        return 0;
    if (text == "Infinity"sv || text == "+Infinity"sv)
        return INFINITY;
    if (text == "-Infinity"sv)
        return -INFINITY;

    auto result = parse_number_text(text);

    // 3. If literal is a List of errors, return NaN.
    if (!result.has_value())
        return NAN;

    // 4. Return StringNumericValue of literal.
    if (result->base != 10) {
        auto bigint = MUST(Crypto::UnsignedBigInteger::from_base(result->base, result->literal));
        return bigint.to_double();
    }

    auto maybe_double = text.to_number<double>(AK::TrimWhitespace::No);
    if (!maybe_double.has_value())
        return NAN;

    return *maybe_double;
}

// 7.1.4 ToNumber ( argument ), https://tc39.es/ecma262/#sec-tonumber
ThrowCompletionOr<Value> Value::to_number_slow_case(VM& vm) const
{
    VERIFY(!is_empty());

    // 1. If argument is a Number, return argument.
    if (is_number())
        return *this;

    switch (m_value.tag) {
    // 2. If argument is either a Symbol or a BigInt, throw a TypeError exception.
    case SYMBOL_TAG:
        return vm.throw_completion<TypeError>(ErrorType::Convert, "symbol", "number");
    case BIGINT_TAG:
        return vm.throw_completion<TypeError>(ErrorType::Convert, "BigInt", "number");
    // 3. If argument is undefined, return NaN.
    case UNDEFINED_TAG:
        return js_nan();
    // 4. If argument is either null or false, return +0𝔽.
    case NULL_TAG:
        return Value(0);
    // 5. If argument is true, return 1𝔽.
    case BOOLEAN_TAG:
        return Value(as_bool() ? 1 : 0);
    // 6. If argument is a String, return StringToNumber(argument).
    case STRING_TAG:
        return string_to_number(as_string().utf8_string_view());
    // 7. Assert: argument is an Object.
    case OBJECT_TAG: {
        // 8. Let primValue be ? ToPrimitive(argument, number).
        auto primitive_value = TRY(to_primitive(vm, PreferredType::Number));

        // 9. Assert: primValue is not an Object.
        VERIFY(!primitive_value.is_object());

        // 10. Return ? ToNumber(primValue).
        return primitive_value.to_number(vm);
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

static Optional<BigInt*> string_to_bigint(VM& vm, StringView string);

// 7.1.13 ToBigInt ( argument ), https://tc39.es/ecma262/#sec-tobigint
ThrowCompletionOr<GC::Ref<BigInt>> Value::to_bigint(VM& vm) const
{
    // 1. Let prim be ? ToPrimitive(argument, number).
    auto primitive = TRY(to_primitive(vm, PreferredType::Number));

    // 2. Return the value that prim corresponds to in Table 12.

    // Number
    if (primitive.is_number()) {
        // Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::Convert, "number", "BigInt");
    }

    switch (primitive.m_value.tag) {
    // Undefined
    case UNDEFINED_TAG:
        // Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::Convert, "undefined", "BigInt");
    // Null
    case NULL_TAG:
        // Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::Convert, "null", "BigInt");
    // Boolean
    case BOOLEAN_TAG: {
        // Return 1n if prim is true and 0n if prim is false.
        auto value = primitive.as_bool() ? 1 : 0;
        return BigInt::create(vm, Crypto::SignedBigInteger { value });
    }
    // BigInt
    case BIGINT_TAG:
        // Return prim.
        return primitive.as_bigint();
    case STRING_TAG: {
        // 1. Let n be ! StringToBigInt(prim).
        auto bigint = string_to_bigint(vm, primitive.as_string().utf8_string_view());

        // 2. If n is undefined, throw a SyntaxError exception.
        if (!bigint.has_value())
            return vm.throw_completion<SyntaxError>(ErrorType::BigIntInvalidValue, primitive);

        // 3. Return n.
        return *bigint.release_value();
    }
    // Symbol
    case SYMBOL_TAG:
        // Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::Convert, "symbol", "BigInt");
    default:
        VERIFY_NOT_REACHED();
    }
}

struct BigIntParseResult {
    StringView literal;
    u8 base { 10 };
    bool is_negative { false };
};

static Optional<BigIntParseResult> parse_bigint_text(StringView text)
{
    BigIntParseResult result {};

    auto parse_for_prefixed_base = [&](auto lower_prefix, auto upper_prefix, auto validator) {
        if (text.length() <= 2)
            return false;
        if (!text.starts_with(lower_prefix) && !text.starts_with(upper_prefix))
            return false;
        return all_of(text.substring_view(2), validator);
    };

    if (parse_for_prefixed_base("0b"sv, "0B"sv, is_ascii_binary_digit)) {
        result.literal = text.substring_view(2);
        result.base = 2;
    } else if (parse_for_prefixed_base("0o"sv, "0O"sv, is_ascii_octal_digit)) {
        result.literal = text.substring_view(2);
        result.base = 8;
    } else if (parse_for_prefixed_base("0x"sv, "0X"sv, is_ascii_hex_digit)) {
        result.literal = text.substring_view(2);
        result.base = 16;
    } else {
        if (text.starts_with('-')) {
            text = text.substring_view(1);
            result.is_negative = true;
        } else if (text.starts_with('+')) {
            text = text.substring_view(1);
        }

        if (!all_of(text, is_ascii_digit))
            return {};

        result.literal = text;
        result.base = 10;
    }

    return result;
}

// 7.1.14 StringToBigInt ( str ), https://tc39.es/ecma262/#sec-stringtobigint
static Optional<BigInt*> string_to_bigint(VM& vm, StringView string)
{
    // 1. Let text be StringToCodePoints(str).
    auto text = Utf8View(string).trim(whitespace_characters, AK::TrimMode::Both).as_string();

    // 2. Let literal be ParseText(text, StringIntegerLiteral).
    auto result = parse_bigint_text(text);

    // 3. If literal is a List of errors, return undefined.
    if (!result.has_value())
        return {};

    // 4. Let mv be the MV of literal.
    // 5. Assert: mv is an integer.
    auto bigint = MUST(Crypto::SignedBigInteger::from_base(result->base, result->literal));
    if (result->is_negative && (bigint != BIGINT_ZERO))
        bigint.negate();

    // 6. Return ℤ(mv).
    return BigInt::create(vm, move(bigint));
}

// 7.1.15 ToBigInt64 ( argument ), https://tc39.es/ecma262/#sec-tobigint64
ThrowCompletionOr<i64> Value::to_bigint_int64(VM& vm) const
{
    // 1. Let n be ? ToBigInt(argument).
    auto bigint = TRY(to_bigint(vm));

    // 2. Let int64bit be ℝ(n) modulo 2^64.
    // 3. If int64bit ≥ 2^63, return ℤ(int64bit - 2^64); otherwise return ℤ(int64bit).
    return static_cast<i64>(bigint->big_integer().to_u64());
}

// 7.1.16 ToBigUint64 ( argument ), https://tc39.es/ecma262/#sec-tobiguint64
ThrowCompletionOr<u64> Value::to_bigint_uint64(VM& vm) const
{
    // 1. Let n be ? ToBigInt(argument).
    auto bigint = TRY(to_bigint(vm));

    // 2. Let int64bit be ℝ(n) modulo 2^64.
    // 3. Return ℤ(int64bit).
    return bigint->big_integer().to_u64();
}

ThrowCompletionOr<double> Value::to_double(VM& vm) const
{
    return TRY(to_number(vm)).as_double();
}

// 7.1.19 ToPropertyKey ( argument ), https://tc39.es/ecma262/#sec-topropertykey
ThrowCompletionOr<PropertyKey> Value::to_property_key(VM& vm) const
{
    // OPTIMIZATION: Return the value as a numeric PropertyKey, if possible.
    if (is_int32() && as_i32() >= 0)
        return PropertyKey { as_i32() };

    // OPTIMIZATION: If this is already a string, we can skip all the ceremony.
    if (is_string())
        return PropertyKey { as_string().utf8_string() };

    // 1. Let key be ? ToPrimitive(argument, string).
    auto key = TRY(to_primitive(vm, PreferredType::String));

    // 2. If key is a Symbol, then
    if (key.is_symbol()) {
        // a. Return key.
        return &key.as_symbol();
    }

    // 3. Return ! ToString(key).
    return MUST(key.to_string(vm));
}

// 7.1.6 ToInt32 ( argument ), https://tc39.es/ecma262/#sec-toint32
ThrowCompletionOr<i32> Value::to_i32_slow_case(VM& vm) const
{
    VERIFY(!is_int32());

    // 1. Let number be ? ToNumber(argument).
    double number = TRY(to_number(vm)).as_double();

    // 2. If number is not finite or number is either +0𝔽 or -0𝔽, return +0𝔽.
    if (!isfinite(number) || number == 0)
        return 0;

    // 3. Let int be the mathematical value whose sign is the sign of number and whose magnitude is floor(abs(ℝ(number))).
    auto abs = fabs(number);
    auto int_val = floor(abs);
    if (signbit(number))
        int_val = -int_val;

    // 4. Let int32bit be int modulo 2^32.
    auto int32bit = modulo(int_val, NumericLimits<u32>::max() + 1.0);

    // 5. If int32bit ≥ 2^31, return 𝔽(int32bit - 2^32); otherwise return 𝔽(int32bit).
    if (int32bit >= 2147483648.0)
        int32bit -= 4294967296.0;
    return static_cast<i32>(int32bit);
}

// 7.1.6 ToInt32 ( argument ), https://tc39.es/ecma262/#sec-toint32
ThrowCompletionOr<i32> Value::to_i32(VM& vm) const
{
    if (is_int32())
        return as_i32();
    return to_i32_slow_case(vm);
}

// 7.1.7 ToUint32 ( argument ), https://tc39.es/ecma262/#sec-touint32
ThrowCompletionOr<u32> Value::to_u32(VM& vm) const
{
    // OPTIMIZATION: If this value is encoded as a positive i32, return it directly.
    if (is_int32() && as_i32() >= 0)
        return as_i32();

    // 1. Let number be ? ToNumber(argument).
    double number = TRY(to_number(vm)).as_double();

    // 2. If number is not finite or number is either +0𝔽 or -0𝔽, return +0𝔽.
    if (!isfinite(number) || number == 0)
        return 0;

    // 3. Let int be the mathematical value whose sign is the sign of number and whose magnitude is floor(abs(ℝ(number))).
    auto int_val = floor(fabs(number));
    if (signbit(number))
        int_val = -int_val;

    // 4. Let int32bit be int modulo 2^32.
    auto int32bit = modulo(int_val, NumericLimits<u32>::max() + 1.0);

    // 5. Return 𝔽(int32bit).
    // Cast to i64 here to ensure that the double --> u32 cast doesn't invoke undefined behavior
    // Otherwise, negative numbers cause a UBSAN warning.
    return static_cast<u32>(static_cast<i64>(int32bit));
}

// 7.1.8 ToInt16 ( argument ), https://tc39.es/ecma262/#sec-toint16
ThrowCompletionOr<i16> Value::to_i16(VM& vm) const
{
    // 1. Let number be ? ToNumber(argument).
    double number = TRY(to_number(vm)).as_double();

    // 2. If number is not finite or number is either +0𝔽 or -0𝔽, return +0𝔽.
    if (!isfinite(number) || number == 0)
        return 0;

    // 3. Let int be the mathematical value whose sign is the sign of number and whose magnitude is floor(abs(ℝ(number))).
    auto abs = fabs(number);
    auto int_val = floor(abs);
    if (signbit(number))
        int_val = -int_val;

    // 4. Let int16bit be int modulo 2^16.
    auto int16bit = modulo(int_val, NumericLimits<u16>::max() + 1.0);

    // 5. If int16bit ≥ 2^15, return 𝔽(int16bit - 2^16); otherwise return 𝔽(int16bit).
    if (int16bit >= 32768.0)
        int16bit -= 65536.0;
    return static_cast<i16>(int16bit);
}

// 7.1.9 ToUint16 ( argument ), https://tc39.es/ecma262/#sec-touint16
ThrowCompletionOr<u16> Value::to_u16(VM& vm) const
{
    // 1. Let number be ? ToNumber(argument).
    double number = TRY(to_number(vm)).as_double();

    // 2. If number is not finite or number is either +0𝔽 or -0𝔽, return +0𝔽.
    if (!isfinite(number) || number == 0)
        return 0;

    // 3. Let int be the mathematical value whose sign is the sign of number and whose magnitude is floor(abs(ℝ(number))).
    auto int_val = floor(fabs(number));
    if (signbit(number))
        int_val = -int_val;

    // 4. Let int16bit be int modulo 2^16.
    auto int16bit = modulo(int_val, NumericLimits<u16>::max() + 1.0);

    // 5. Return 𝔽(int16bit).
    return static_cast<u16>(int16bit);
}

// 7.1.10 ToInt8 ( argument ), https://tc39.es/ecma262/#sec-toint8
ThrowCompletionOr<i8> Value::to_i8(VM& vm) const
{
    // 1. Let number be ? ToNumber(argument).
    double number = TRY(to_number(vm)).as_double();

    // 2. If number is not finite or number is either +0𝔽 or -0𝔽, return +0𝔽.
    if (!isfinite(number) || number == 0)
        return 0;

    // 3. Let int be the mathematical value whose sign is the sign of number and whose magnitude is floor(abs(ℝ(number))).
    auto abs = fabs(number);
    auto int_val = floor(abs);
    if (signbit(number))
        int_val = -int_val;

    // 4. Let int8bit be int modulo 2^8.
    auto int8bit = modulo(int_val, NumericLimits<u8>::max() + 1.0);

    // 5. If int8bit ≥ 2^7, return 𝔽(int8bit - 2^8); otherwise return 𝔽(int8bit).
    if (int8bit >= 128.0)
        int8bit -= 256.0;
    return static_cast<i8>(int8bit);
}

// 7.1.11 ToUint8 ( argument ), https://tc39.es/ecma262/#sec-touint8
ThrowCompletionOr<u8> Value::to_u8(VM& vm) const
{
    // 1. Let number be ? ToNumber(argument).
    double number = TRY(to_number(vm)).as_double();

    // 2. If number is not finite or number is either +0𝔽 or -0𝔽, return +0𝔽.
    if (!isfinite(number) || number == 0)
        return 0;

    // 3. Let int be the mathematical value whose sign is the sign of number and whose magnitude is floor(abs(ℝ(number))).
    auto int_val = floor(fabs(number));
    if (signbit(number))
        int_val = -int_val;

    // 4. Let int8bit be int modulo 2^8.
    auto int8bit = modulo(int_val, NumericLimits<u8>::max() + 1.0);

    // 5. Return 𝔽(int8bit).
    return static_cast<u8>(int8bit);
}

// 7.1.12 ToUint8Clamp ( argument ), https://tc39.es/ecma262/#sec-touint8clamp
ThrowCompletionOr<u8> Value::to_u8_clamp(VM& vm) const
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(to_number(vm));

    // 2. If number is NaN, return +0𝔽.
    if (number.is_nan())
        return 0;

    double value = number.as_double();

    // 3. If ℝ(number) ≤ 0, return +0𝔽.
    if (value <= 0.0)
        return 0;

    // 4. If ℝ(number) ≥ 255, return 255𝔽.
    if (value >= 255.0)
        return 255;

    // 5. Let f be floor(ℝ(number)).
    auto int_val = floor(value);

    // 6. If f + 0.5 < ℝ(number), return 𝔽(f + 1).
    if (int_val + 0.5 < value)
        return static_cast<u8>(int_val + 1.0);

    // 7. If ℝ(number) < f + 0.5, return 𝔽(f).
    if (value < int_val + 0.5)
        return static_cast<u8>(int_val);

    // 8. If f is odd, return 𝔽(f + 1).
    if (fmod(int_val, 2.0) == 1.0)
        return static_cast<u8>(int_val + 1.0);

    // 9. Return 𝔽(f).
    return static_cast<u8>(int_val);
}

// 7.1.20 ToLength ( argument ), https://tc39.es/ecma262/#sec-tolength
ThrowCompletionOr<size_t> Value::to_length(VM& vm) const
{
    // 1. Let len be ? ToIntegerOrInfinity(argument).
    auto len = TRY(to_integer_or_infinity(vm));

    // 2. If len ≤ 0, return +0𝔽.
    if (len <= 0)
        return 0;

    // FIXME: The expected output range is 0 - 2^53-1, but we don't want to overflow the size_t on 32-bit platforms.
    //        Convert this to u64 so it works everywhere.
    constexpr double length_limit = sizeof(void*) == 4 ? NumericLimits<size_t>::max() : MAX_ARRAY_LIKE_INDEX;

    // 3. Return 𝔽(min(len, 2^53 - 1)).
    return min(len, length_limit);
}

// 7.1.22 ToIndex ( argument ), https://tc39.es/ecma262/#sec-toindex
ThrowCompletionOr<size_t> Value::to_index(VM& vm) const
{
    // 1. If value is undefined, then
    if (is_undefined()) {
        // a. Return 0.
        return 0;
    }

    // 2. Else,
    // a. Let integer be ? ToIntegerOrInfinity(value).
    auto integer = TRY(to_integer_or_infinity(vm));

    // OPTIMIZATION: If the value is negative, ToLength normalizes it to 0, and we fail the SameValue comparison below.
    //               Bail out early instead.
    if (integer < 0)
        return vm.throw_completion<RangeError>(ErrorType::InvalidIndex);

    // b. Let clamped be ! ToLength(𝔽(integer)).
    auto clamped = MUST(Value(integer).to_length(vm));

    // c. If SameValue(𝔽(integer), clamped) is false, throw a RangeError exception.
    if (integer != clamped)
        return vm.throw_completion<RangeError>(ErrorType::InvalidIndex);

    // d. Assert: 0 ≤ integer ≤ 2^53 - 1.
    VERIFY(0 <= integer && integer <= MAX_ARRAY_LIKE_INDEX);

    // e. Return integer.
    // NOTE: We return the clamped value here, which already has the right type.
    return clamped;
}

// 7.1.5 ToIntegerOrInfinity ( argument ), https://tc39.es/ecma262/#sec-tointegerorinfinity
ThrowCompletionOr<double> Value::to_integer_or_infinity(VM& vm) const
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(to_number(vm));

    // 2. If number is NaN, +0𝔽, or -0𝔽, return 0.
    if (number.is_nan() || number.as_double() == 0)
        return 0;

    // 3. If number is +∞𝔽, return +∞.
    // 4. If number is -∞𝔽, return -∞.
    if (number.is_infinity())
        return number.as_double();

    // 5. Let integer be floor(abs(ℝ(number))).
    auto integer = floor(fabs(number.as_double()));

    // 6. If number < -0𝔽, set integer to -integer.
    // NOTE: The zero check is required as 'integer' is a double here but an MV in the spec,
    //       which doesn't have negative zero.
    if (number.as_double() < 0 && integer != 0)
        integer = -integer;

    // 7. Return integer.
    return integer;
}

// Standalone variant using plain doubles for cases where we already got numbers and know the AO won't throw.
double to_integer_or_infinity(double number)
{
    // 1. Let number be ? ToNumber(argument).

    // 2. If number is NaN, +0𝔽, or -0𝔽, return 0.
    if (isnan(number) || number == 0)
        return 0;

    // 3. If number is +∞𝔽, return +∞.
    if (__builtin_isinf_sign(number) > 0)
        return static_cast<double>(INFINITY);

    // 4. If number is -∞𝔽, return -∞.
    if (__builtin_isinf_sign(number) < 0)
        return static_cast<double>(-INFINITY);

    // 5. Let integer be floor(abs(ℝ(number))).
    auto integer = floor(fabs(number));

    // 6. If number < -0𝔽, set integer to -integer.
    // NOTE: The zero check is required as 'integer' is a double here but an MV in the spec,
    //       which doesn't have negative zero.
    if (number < 0 && integer != 0)
        integer = -integer;

    // 7. Return integer.
    return integer;
}

// 7.3.3 GetV ( V, P ), https://tc39.es/ecma262/#sec-getv
ThrowCompletionOr<Value> Value::get(VM& vm, PropertyKey const& property_key) const
{
    // 1. Let O be ? ToObject(V).
    auto object = TRY([&]() -> ThrowCompletionOr<GC::Ref<Object>> {
        // OPTIMIZATION: For primitive values, we can skip allocation of temporary object and look
        //               directly into prototype where requested property is located.
        if (is_string() && property_key != vm.names.length)
            return vm.current_realm()->intrinsics().string_prototype();
        if (is_boolean())
            return vm.current_realm()->intrinsics().boolean_prototype();
        if (is_number())
            return vm.current_realm()->intrinsics().number_prototype();
        if (is_bigint())
            return vm.current_realm()->intrinsics().bigint_prototype();
        if (is_symbol())
            return vm.current_realm()->intrinsics().symbol_prototype();
        return to_object(vm);
    }());

    // 2. Return ? O.[[Get]](P, V).
    return TRY(object->internal_get(property_key, *this));
}

// 7.3.11 GetMethod ( V, P ), https://tc39.es/ecma262/#sec-getmethod
ThrowCompletionOr<GC::Ptr<FunctionObject>> Value::get_method(VM& vm, PropertyKey const& property_key) const
{
    // 1. Let func be ? GetV(V, P).
    auto function = TRY(get(vm, property_key));

    // 2. If func is either undefined or null, return undefined.
    if (function.is_nullish())
        return nullptr;

    // 3. If IsCallable(func) is false, throw a TypeError exception.
    if (!function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, function.to_string_without_side_effects());

    // 4. Return func.
    return function.as_function();
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression > ShiftExpression
ThrowCompletionOr<Value> greater_than(VM& vm, Value lhs, Value rhs)
{
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // OPTIMIZATION: If both values are i32, we can do a direct comparison without calling into IsLessThan.
    if (lhs.is_int32() && rhs.is_int32())
        return lhs.as_i32() > rhs.as_i32();

    // 5. Let r be ? IsLessThan(rval, lval, false).
    auto relation = TRY(is_less_than(vm, lhs, rhs, false));

    // 6. If r is undefined, return false. Otherwise, return r.
    if (relation == TriState::Unknown)
        return Value(false);
    return Value(relation == TriState::True);
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression >= ShiftExpression
ThrowCompletionOr<Value> greater_than_equals(VM& vm, Value lhs, Value rhs)
{
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // OPTIMIZATION: If both values are i32, we can do a direct comparison without calling into IsLessThan.
    if (lhs.is_int32() && rhs.is_int32())
        return lhs.as_i32() >= rhs.as_i32();

    // 5. Let r be ? IsLessThan(lval, rval, true).
    auto relation = TRY(is_less_than(vm, lhs, rhs, true));

    // 6. If r is true or undefined, return false. Otherwise, return true.
    if (relation == TriState::Unknown || relation == TriState::True)
        return Value(false);
    return Value(true);
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression < ShiftExpression
ThrowCompletionOr<Value> less_than(VM& vm, Value lhs, Value rhs)
{
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // OPTIMIZATION: If both values are i32, we can do a direct comparison without calling into IsLessThan.
    if (lhs.is_int32() && rhs.is_int32())
        return lhs.as_i32() < rhs.as_i32();

    // 5. Let r be ? IsLessThan(lval, rval, true).
    auto relation = TRY(is_less_than(vm, lhs, rhs, true));

    // 6. If r is undefined, return false. Otherwise, return r.
    if (relation == TriState::Unknown)
        return Value(false);
    return Value(relation == TriState::True);
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression <= ShiftExpression
ThrowCompletionOr<Value> less_than_equals(VM& vm, Value lhs, Value rhs)
{
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // OPTIMIZATION: If both values are i32, we can do a direct comparison without calling into IsLessThan.
    if (lhs.is_int32() && rhs.is_int32())
        return lhs.as_i32() <= rhs.as_i32();

    // 5. Let r be ? IsLessThan(rval, lval, false).
    auto relation = TRY(is_less_than(vm, lhs, rhs, false));

    // 6. If r is true or undefined, return false. Otherwise, return true.
    if (relation == TriState::True || relation == TriState::Unknown)
        return Value(false);
    return Value(true);
}

// 13.12 Binary Bitwise Operators, https://tc39.es/ecma262/#sec-binary-bitwise-operators
// BitwiseANDExpression : BitwiseANDExpression & EqualityExpression
ThrowCompletionOr<Value> bitwise_and(VM& vm, Value lhs, Value rhs)
{
    // OPTIMIZATION: Fast path when both values are Int32.
    if (lhs.is_int32() && rhs.is_int32())
        return Value(lhs.as_i32() & rhs.as_i32());

    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.17 Number::bitwiseAND ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseAND
        // 1. Return NumberBitwiseOp(&, x, y).
        if (!lhs_numeric.is_finite_number() || !rhs_numeric.is_finite_number())
            return Value(0);
        return Value(TRY(lhs_numeric.to_i32(vm)) & TRY(rhs_numeric.to_i32(vm)));
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.18 BigInt::bitwiseAND ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-bitwiseAND
        // 1. Return BigIntBitwiseOp(&, x, y).
        return BigInt::create(vm, lhs_numeric.as_bigint().big_integer().bitwise_and(rhs_numeric.as_bigint().big_integer()));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "bitwise AND");
}

// 13.12 Binary Bitwise Operators, https://tc39.es/ecma262/#sec-binary-bitwise-operators
// BitwiseORExpression : BitwiseORExpression | BitwiseXORExpression
ThrowCompletionOr<Value> bitwise_or(VM& vm, Value lhs, Value rhs)
{
    // OPTIMIZATION: Fast path when both values are Int32.
    if (lhs.is_int32() && rhs.is_int32())
        return Value(lhs.as_i32() | rhs.as_i32());

    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.19 Number::bitwiseOR ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseOR
        // 1. Return NumberBitwiseOp(|, x, y).
        if (!lhs_numeric.is_finite_number() && !rhs_numeric.is_finite_number())
            return Value(0);
        if (!lhs_numeric.is_finite_number())
            return rhs_numeric;
        if (!rhs_numeric.is_finite_number())
            return lhs_numeric;
        return Value(TRY(lhs_numeric.to_i32(vm)) | TRY(rhs_numeric.to_i32(vm)));
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.20 BigInt::bitwiseOR ( x, y )
        // 1. Return BigIntBitwiseOp(|, x, y).
        return BigInt::create(vm, lhs_numeric.as_bigint().big_integer().bitwise_or(rhs_numeric.as_bigint().big_integer()));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "bitwise OR");
}

// 13.12 Binary Bitwise Operators, https://tc39.es/ecma262/#sec-binary-bitwise-operators
// BitwiseXORExpression : BitwiseXORExpression ^ BitwiseANDExpression
ThrowCompletionOr<Value> bitwise_xor(VM& vm, Value lhs, Value rhs)
{
    // OPTIMIZATION: Fast path when both values are Int32.
    if (lhs.is_int32() && rhs.is_int32())
        return Value(lhs.as_i32() ^ rhs.as_i32());

    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.18 Number::bitwiseXOR ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseXOR
        // 1. Return NumberBitwiseOp(^, x, y).
        if (!lhs_numeric.is_finite_number() && !rhs_numeric.is_finite_number())
            return Value(0);
        if (!lhs_numeric.is_finite_number())
            return rhs_numeric;
        if (!rhs_numeric.is_finite_number())
            return lhs_numeric;
        return Value(TRY(lhs_numeric.to_i32(vm)) ^ TRY(rhs_numeric.to_i32(vm)));
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.19 BigInt::bitwiseXOR ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-bitwiseXOR
        // 1. Return BigIntBitwiseOp(^, x, y).
        return BigInt::create(vm, lhs_numeric.as_bigint().big_integer().bitwise_xor(rhs_numeric.as_bigint().big_integer()));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "bitwise XOR");
}

// 13.5.6 Bitwise NOT Operator ( ~ ), https://tc39.es/ecma262/#sec-bitwise-not-operator
// UnaryExpression : ~ UnaryExpression
ThrowCompletionOr<Value> bitwise_not(VM& vm, Value lhs)
{
    // 1. Let expr be ? Evaluation of UnaryExpression.
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 2. Let oldValue be ? ToNumeric(? GetValue(expr)).

    auto old_value = TRY(lhs.to_numeric(vm));

    // 3. If oldValue is a Number, then
    if (old_value.is_number()) {
        // a. Return Number::bitwiseNOT(oldValue).

        // 6.1.6.1.2 Number::bitwiseNOT ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseNOT
        // 1. Let oldValue be ! ToInt32(x).
        // 2. Return the result of applying bitwise complement to oldValue. The mathematical value of the result is
        //    exactly representable as a 32-bit two's complement bit string.
        return Value(~TRY(old_value.to_i32(vm)));
    }

    // 4. Else,
    // a. Assert: oldValue is a BigInt.
    VERIFY(old_value.is_bigint());

    // b. Return BigInt::bitwiseNOT(oldValue).

    // 6.1.6.2.2 BigInt::bitwiseNOT ( x ), https://tc39.es/ecma262/#sec-numeric-types-bigint-bitwiseNOT
    // 1. Return -x - 1ℤ.
    return BigInt::create(vm, old_value.as_bigint().big_integer().bitwise_not());
}

// 13.5.4 Unary + Operator, https://tc39.es/ecma262/#sec-unary-plus-operator
// UnaryExpression : + UnaryExpression
ThrowCompletionOr<Value> unary_plus(VM& vm, Value lhs)
{
    // 1. Let expr be ? Evaluation of UnaryExpression.
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 2. Return ? ToNumber(? GetValue(expr)).
    return TRY(lhs.to_number(vm));
}

// 13.5.5 Unary - Operator, https://tc39.es/ecma262/#sec-unary-minus-operator
// UnaryExpression : - UnaryExpression
ThrowCompletionOr<Value> unary_minus(VM& vm, Value lhs)
{
    // 1. Let expr be ? Evaluation of UnaryExpression.
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 2. Let oldValue be ? ToNumeric(? GetValue(expr)).
    auto old_value = TRY(lhs.to_numeric(vm));

    // 3. If oldValue is a Number, then
    if (old_value.is_number()) {
        // a. Return Number::unaryMinus(oldValue).

        // 6.1.6.1.1 Number::unaryMinus ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-unaryMinus
        // 1. If x is NaN, return NaN.
        if (old_value.is_nan())
            return js_nan();

        // 2. Return the result of negating x; that is, compute a Number with the same magnitude but opposite sign.
        return Value(-old_value.as_double());
    }

    // 4. Else,
    // a. Assert: oldValue is a BigInt.
    VERIFY(old_value.is_bigint());

    // b. Return BigInt::unaryMinus(oldValue).

    // 6.1.6.2.1 BigInt::unaryMinus ( x ), https://tc39.es/ecma262/#sec-numeric-types-bigint-unaryMinus
    // 1. If x is 0ℤ, return 0ℤ.
    if (old_value.as_bigint().big_integer() == BIGINT_ZERO)
        return BigInt::create(vm, BIGINT_ZERO);

    // 2. Return the BigInt value that represents the negation of ℝ(x).
    auto big_integer_negated = old_value.as_bigint().big_integer();
    big_integer_negated.negate();
    return BigInt::create(vm, big_integer_negated);
}

// 13.9.1 The Left Shift Operator ( << ), https://tc39.es/ecma262/#sec-left-shift-operator
// ShiftExpression : ShiftExpression << AdditiveExpression
ThrowCompletionOr<Value> left_shift(VM& vm, Value lhs, Value rhs)
{
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.9 Number::leftShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-leftShift

        // OPTIMIZATION: Handle infinite values according to the results returned by ToInt32/ToUint32.
        if (!lhs_numeric.is_finite_number())
            return Value(0);
        if (!rhs_numeric.is_finite_number())
            return lhs_numeric;

        // 1. Let lnum be ! ToInt32(x).
        auto lhs_i32 = MUST(lhs_numeric.to_i32(vm));

        // 2. Let rnum be ! ToUint32(y).
        auto rhs_u32 = MUST(rhs_numeric.to_u32(vm));

        // 3. Let shiftCount be ℝ(rnum) modulo 32.
        auto shift_count = rhs_u32 % 32;

        // 4. Return the result of left shifting lnum by shiftCount bits. The mathematical value of the result is
        //    exactly representable as a 32-bit two's complement bit string.
        return Value(lhs_i32 << shift_count);
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.9 BigInt::leftShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-leftShift
        auto multiplier_divisor = Crypto::SignedBigInteger { Crypto::NumberTheory::Power(Crypto::UnsignedBigInteger(2), rhs_numeric.as_bigint().big_integer().unsigned_value()) };

        // 1. If y < 0ℤ, then
        if (rhs_numeric.as_bigint().big_integer().is_negative()) {
            // a. Return the BigInt value that represents ℝ(x) / 2^-y, rounding down to the nearest integer, including for negative numbers.
            // NOTE: Since y is negative we can just do ℝ(x) / 2^|y|
            auto const& big_integer = lhs_numeric.as_bigint().big_integer();
            auto division_result = big_integer.divided_by(multiplier_divisor);

            // For positive initial values and no remainder just return quotient
            if (division_result.remainder.is_zero() || !big_integer.is_negative())
                return BigInt::create(vm, division_result.quotient);
            // For negative round "down" to the next negative number
            return BigInt::create(vm, division_result.quotient.minus(Crypto::SignedBigInteger { 1 }));
        }
        // 2. Return the BigInt value that represents ℝ(x) × 2^y.
        return Value(BigInt::create(vm, lhs_numeric.as_bigint().big_integer().multiplied_by(multiplier_divisor)));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "left-shift");
}

// 13.9.2 The Signed Right Shift Operator ( >> ), https://tc39.es/ecma262/#sec-signed-right-shift-operator
// ShiftExpression : ShiftExpression >> AdditiveExpression
ThrowCompletionOr<Value> right_shift(VM& vm, Value lhs, Value rhs)
{
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.10 Number::signedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-signedRightShift

        // OPTIMIZATION: Handle infinite values according to the results returned by ToInt32/ToUint32.
        if (!lhs_numeric.is_finite_number())
            return Value(0);
        if (!rhs_numeric.is_finite_number())
            return lhs_numeric;

        // 1. Let lnum be ! ToInt32(x).
        auto lhs_i32 = MUST(lhs_numeric.to_i32(vm));

        // 2. Let rnum be ! ToUint32(y).
        auto rhs_u32 = MUST(rhs_numeric.to_u32(vm));

        // 3. Let shiftCount be ℝ(rnum) modulo 32.
        auto shift_count = rhs_u32 % 32;

        // 4. Return the result of performing a sign-extending right shift of lnum by shiftCount bits.
        //    The most significant bit is propagated. The mathematical value of the result is exactly representable
        //    as a 32-bit two's complement bit string.
        return Value(lhs_i32 >> shift_count);
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.10 BigInt::signedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-signedRightShift
        // 1. Return BigInt::leftShift(x, -y).
        auto rhs_negated = rhs_numeric.as_bigint().big_integer();
        rhs_negated.negate();
        return left_shift(vm, lhs, BigInt::create(vm, rhs_negated));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "right-shift");
}

// 13.9.3 The Unsigned Right Shift Operator ( >>> ), https://tc39.es/ecma262/#sec-unsigned-right-shift-operator
// ShiftExpression : ShiftExpression >>> AdditiveExpression
ThrowCompletionOr<Value> unsigned_right_shift(VM& vm, Value lhs, Value rhs)
{
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 5-6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.11 Number::unsignedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-unsignedRightShift

        // OPTIMIZATION: Handle infinite values according to the results returned by ToUint32.
        if (!lhs_numeric.is_finite_number())
            return Value(0);
        if (!rhs_numeric.is_finite_number())
            return lhs_numeric;

        // 1. Let lnum be ! ToUint32(x).
        auto lhs_u32 = MUST(lhs_numeric.to_u32(vm));

        // 2. Let rnum be ! ToUint32(y).
        auto rhs_u32 = MUST(rhs_numeric.to_u32(vm));

        // 3. Let shiftCount be ℝ(rnum) modulo 32.
        auto shift_count = rhs_u32 % 32;

        // 4. Return the result of performing a zero-filling right shift of lnum by shiftCount bits.
        //    Vacated bits are filled with zero. The mathematical value of the result is exactly representable
        //    as a 32-bit unsigned bit string.
        return Value(lhs_u32 >> shift_count);
    }

    // 6. If lnum is a BigInt, then
    // d. If opText is >>>, return ? BigInt::unsignedRightShift(lnum, rnum).

    // 6.1.6.2.11 BigInt::unsignedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-unsignedRightShift
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperator, "unsigned right-shift");
}

// 13.8.1 The Addition Operator ( + ), https://tc39.es/ecma262/#sec-addition-operator-plus
// AdditiveExpression : AdditiveExpression + MultiplicativeExpression
ThrowCompletionOr<Value> add(VM& vm, Value lhs, Value rhs)
{
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator

    // 1. If opText is +, then

    // OPTIMIZATION: If both values are i32 or double, we can do a direct addition without the type conversions below.
    if (both_number(lhs, rhs)) {
        if (lhs.is_int32() && rhs.is_int32()) {
            Checked<i32> result;
            result = MUST(lhs.to_i32(vm));
            result += MUST(rhs.to_i32(vm));
            if (!result.has_overflow())
                return Value(result.value());
        }
        return Value(lhs.as_double() + rhs.as_double());
    }

    // a. Let lprim be ? ToPrimitive(lval).
    auto lhs_primitive = TRY(lhs.to_primitive(vm));

    // b. Let rprim be ? ToPrimitive(rval).
    auto rhs_primitive = TRY(rhs.to_primitive(vm));

    // c. If lprim is a String or rprim is a String, then
    if (lhs_primitive.is_string() || rhs_primitive.is_string()) {
        // i. Let lstr be ? ToString(lprim).
        auto lhs_string = TRY(lhs_primitive.to_primitive_string(vm));

        // ii. Let rstr be ? ToString(rprim).
        auto rhs_string = TRY(rhs_primitive.to_primitive_string(vm));

        // iii. Return the string-concatenation of lstr and rstr.
        return PrimitiveString::create(vm, lhs_string, rhs_string);
    }

    // d. Set lval to lprim.
    // e. Set rval to rprim.

    // 2. NOTE: At this point, it must be a numeric operation.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs_primitive.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs_primitive.to_numeric(vm));

    // 6. N/A.

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.7 Number::add ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-add
        auto x = lhs_numeric.as_double();
        auto y = rhs_numeric.as_double();
        return Value(x + y);
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.7 BigInt::add ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-add
        auto x = lhs_numeric.as_bigint().big_integer();
        auto y = rhs_numeric.as_bigint().big_integer();
        return BigInt::create(vm, x.plus(y));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "addition");
}

// 13.8.2 The Subtraction Operator ( - ), https://tc39.es/ecma262/#sec-subtraction-operator-minus
// AdditiveExpression : AdditiveExpression - MultiplicativeExpression
ThrowCompletionOr<Value> sub(VM& vm, Value lhs, Value rhs)
{
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.8 Number::subtract ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-subtract
        auto x = lhs_numeric.as_double();
        auto y = rhs_numeric.as_double();
        // 1. Return Number::add(x, Number::unaryMinus(y)).
        return Value(x - y);
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.8 BigInt::subtract ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-subtract
        auto x = lhs_numeric.as_bigint().big_integer();
        auto y = rhs_numeric.as_bigint().big_integer();
        // 1. Return the BigInt value that represents the difference x minus y.
        return BigInt::create(vm, x.minus(y));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "subtraction");
}

// 13.7 Multiplicative Operators, https://tc39.es/ecma262/#sec-multiplicative-operators
// MultiplicativeExpression : MultiplicativeExpression MultiplicativeOperator ExponentiationExpression
ThrowCompletionOr<Value> mul(VM& vm, Value lhs, Value rhs)
{
    // OPTIMIZATION: Fast path for multiplication of two Int32 values.
    if (lhs.is_int32() && rhs.is_int32()) {
        Checked<i32> result = lhs.as_i32();
        result *= rhs.as_i32();
        if (!result.has_overflow())
            return result.value();
    }

    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.4 Number::multiply ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-multiply
        auto x = lhs_numeric.as_double();
        auto y = rhs_numeric.as_double();
        return Value(x * y);
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.4 BigInt::multiply ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-multiply
        auto x = lhs_numeric.as_bigint().big_integer();
        auto y = rhs_numeric.as_bigint().big_integer();
        // 1. Return the BigInt value that represents the product of x and y.
        return BigInt::create(vm, x.multiplied_by(y));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "multiplication");
}

// 13.7 Multiplicative Operators, https://tc39.es/ecma262/#sec-multiplicative-operators
// MultiplicativeExpression : MultiplicativeExpression MultiplicativeOperator ExponentiationExpression
ThrowCompletionOr<Value> div(VM& vm, Value lhs, Value rhs)
{
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.5 Number::divide ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-divide
        return Value(lhs_numeric.as_double() / rhs_numeric.as_double());
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.5 BigInt::divide ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-divide
        auto x = lhs_numeric.as_bigint().big_integer();
        auto y = rhs_numeric.as_bigint().big_integer();
        // 1. If y is 0ℤ, throw a RangeError exception.
        if (y == BIGINT_ZERO)
            return vm.throw_completion<RangeError>(ErrorType::DivisionByZero);
        // 2. Let quotient be ℝ(x) / ℝ(y).
        // 3. Return the BigInt value that represents quotient rounded towards 0 to the next integer value.
        return BigInt::create(vm, x.divided_by(y).quotient);
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "division");
}

// 13.7 Multiplicative Operators, https://tc39.es/ecma262/#sec-multiplicative-operators
// MultiplicativeExpression : MultiplicativeExpression MultiplicativeOperator ExponentiationExpression
ThrowCompletionOr<Value> mod(VM& vm, Value lhs, Value rhs)
{
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        // 6.1.6.1.6 Number::remainder ( n, d ), https://tc39.es/ecma262/#sec-numeric-types-number-remainder
        // The ECMA specification is describing the mathematical definition of modulus
        // implemented by fmod.
        auto n = lhs_numeric.as_double();
        auto d = rhs_numeric.as_double();
        return Value(fmod(n, d));
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.6 BigInt::remainder ( n, d ), https://tc39.es/ecma262/#sec-numeric-types-bigint-remainder
        auto n = lhs_numeric.as_bigint().big_integer();
        auto d = rhs_numeric.as_bigint().big_integer();
        // 1. If d is 0ℤ, throw a RangeError exception.
        if (d == BIGINT_ZERO)
            return vm.throw_completion<RangeError>(ErrorType::DivisionByZero);
        // 2. If n is 0ℤ, return 0ℤ.
        // 3. Let quotient be ℝ(n) / ℝ(d).
        // 4. Let q be the BigInt whose sign is the sign of quotient and whose magnitude is floor(abs(quotient)).
        // 5. Return n - (d × q).
        return BigInt::create(vm, n.divided_by(d).remainder);
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "modulo");
}

// 6.1.6.1.3 Number::exponentiate ( base, exponent ), https://tc39.es/ecma262/#sec-numeric-types-number-exponentiate
static Value exp_double(Value base, Value exponent)
{
    VERIFY(both_number(base, exponent));

    // 1. If exponent is NaN, return NaN.
    if (exponent.is_nan())
        return js_nan();

    // 2. If exponent is +0𝔽 or exponent is -0𝔽, return 1𝔽.
    if (exponent.is_positive_zero() || exponent.is_negative_zero())
        return Value(1);

    // 3. If base is NaN, return NaN.
    if (base.is_nan())
        return js_nan();

    // 4. If base is +∞𝔽, then
    if (base.is_positive_infinity()) {
        // a. If exponent > +0𝔽, return +∞𝔽. Otherwise, return +0𝔽.
        return exponent.as_double() > 0 ? js_infinity() : Value(0);
    }

    // 5. If base is -∞𝔽, then
    if (base.is_negative_infinity()) {
        auto is_odd_integral_number = exponent.is_integral_number() && (fmod(exponent.as_double(), 2.0) != 0);

        // a. If exponent > +0𝔽, then
        if (exponent.as_double() > 0) {
            // i. If exponent is an odd integral Number, return -∞𝔽. Otherwise, return +∞𝔽.
            return is_odd_integral_number ? js_negative_infinity() : js_infinity();
        }
        // b. Else,
        else {
            // i. If exponent is an odd integral Number, return -0𝔽. Otherwise, return +0𝔽.
            return is_odd_integral_number ? Value(-0.0) : Value(0);
        }
    }

    // 6. If base is +0𝔽, then
    if (base.is_positive_zero()) {
        // a. If exponent > +0𝔽, return +0𝔽. Otherwise, return +∞𝔽.
        return exponent.as_double() > 0 ? Value(0) : js_infinity();
    }

    // 7. If base is -0𝔽, then
    if (base.is_negative_zero()) {
        auto is_odd_integral_number = exponent.is_integral_number() && (fmod(exponent.as_double(), 2.0) != 0);

        // a. If exponent > +0𝔽, then
        if (exponent.as_double() > 0) {
            // i. If exponent is an odd integral Number, return -0𝔽. Otherwise, return +0𝔽.
            return is_odd_integral_number ? Value(-0.0) : Value(0);
        }
        // b. Else,
        else {
            // i. If exponent is an odd integral Number, return -∞𝔽. Otherwise, return +∞𝔽.
            return is_odd_integral_number ? js_negative_infinity() : js_infinity();
        }
    }

    // 8. Assert: base is finite and is neither +0𝔽 nor -0𝔽.
    VERIFY(base.is_finite_number() && !base.is_positive_zero() && !base.is_negative_zero());

    // 9. If exponent is +∞𝔽, then
    if (exponent.is_positive_infinity()) {
        auto absolute_base = fabs(base.as_double());

        // a. If abs(ℝ(base)) > 1, return +∞𝔽.
        if (absolute_base > 1)
            return js_infinity();
        // b. If abs(ℝ(base)) is 1, return NaN.
        else if (absolute_base == 1)
            return js_nan();
        // c. If abs(ℝ(base)) < 1, return +0𝔽.
        else if (absolute_base < 1)
            return Value(0);
    }

    // 10. If exponent is -∞𝔽, then
    if (exponent.is_negative_infinity()) {
        auto absolute_base = fabs(base.as_double());

        // a. If abs(ℝ(base)) > 1, return +0𝔽.
        if (absolute_base > 1)
            return Value(0);
        // b. If abs(ℝ(base)) is 1, return NaN.
        else if (absolute_base == 1)
            return js_nan();
        // a. If abs(ℝ(base)) > 1, return +0𝔽.
        else if (absolute_base < 1)
            return js_infinity();
    }

    // 11. Assert: exponent is finite and is neither +0𝔽 nor -0𝔽.
    VERIFY(exponent.is_finite_number() && !exponent.is_positive_zero() && !exponent.is_negative_zero());

    // 12. If base < -0𝔽 and exponent is not an integral Number, return NaN.
    if (base.as_double() < 0 && !exponent.is_integral_number())
        return js_nan();

    // 13. Return an implementation-approximated Number value representing the result of raising ℝ(base) to the ℝ(exponent) power.
    return Value(::pow(base.as_double(), exponent.as_double()));
}

// 13.6 Exponentiation Operator, https://tc39.es/ecma262/#sec-exp-operator
// ExponentiationExpression : UpdateExpression ** ExponentiationExpression
ThrowCompletionOr<Value> exp(VM& vm, Value lhs, Value rhs)
{
    // 3. Let lnum be ? ToNumeric(lval).
    auto lhs_numeric = TRY(lhs.to_numeric(vm));

    // 4. Let rnum be ? ToNumeric(rval).
    auto rhs_numeric = TRY(rhs.to_numeric(vm));

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if (both_number(lhs_numeric, rhs_numeric)) {
        return exp_double(lhs_numeric, rhs_numeric);
    }
    if (both_bigint(lhs_numeric, rhs_numeric)) {
        // 6.1.6.2.3 BigInt::exponentiate ( base, exponent ), https://tc39.es/ecma262/#sec-numeric-types-bigint-exponentiate
        auto base = lhs_numeric.as_bigint().big_integer();
        auto exponent = rhs_numeric.as_bigint().big_integer();
        // 1. If exponent < 0ℤ, throw a RangeError exception.
        if (exponent.is_negative())
            return vm.throw_completion<RangeError>(ErrorType::NegativeExponent);
        // 2. If base is 0ℤ and exponent is 0ℤ, return 1ℤ.
        // 3. Return the BigInt value that represents ℝ(base) raised to the power ℝ(exponent).
        return BigInt::create(vm, Crypto::NumberTheory::Power(base, exponent));
    }
    return vm.throw_completion<TypeError>(ErrorType::BigIntBadOperatorOtherType, "exponentiation");
}

ThrowCompletionOr<Value> in(VM& vm, Value lhs, Value rhs)
{
    if (!rhs.is_object())
        return vm.throw_completion<TypeError>(ErrorType::InOperatorWithObject);
    auto lhs_property_key = TRY(lhs.to_property_key(vm));
    return Value(TRY(rhs.as_object().has_property(lhs_property_key)));
}

// 13.10.2 InstanceofOperator ( V, target ), https://tc39.es/ecma262/#sec-instanceofoperator
ThrowCompletionOr<Value> instance_of(VM& vm, Value value, Value target)
{
    // 1. If target is not an Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let instOfHandler be ? GetMethod(target, @@hasInstance).
    auto instance_of_handler = TRY(target.get_method(vm, vm.well_known_symbol_has_instance()));

    // 3. If instOfHandler is not undefined, then
    if (instance_of_handler) {
        // a. Return ToBoolean(? Call(instOfHandler, target, « V »)).
        return Value(TRY(call(vm, *instance_of_handler, target, value)).to_boolean());
    }

    // 4. If IsCallable(target) is false, throw a TypeError exception.
    if (!target.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, target.to_string_without_side_effects());

    // 5. Return ? OrdinaryHasInstance(target, V).
    return ordinary_has_instance(vm, target, value);
}

// 7.3.22 OrdinaryHasInstance ( C, O ), https://tc39.es/ecma262/#sec-ordinaryhasinstance
ThrowCompletionOr<Value> ordinary_has_instance(VM& vm, Value lhs, Value rhs)
{
    // 1. If IsCallable(C) is false, return false.
    if (!rhs.is_function())
        return Value(false);

    auto& rhs_function = rhs.as_function();

    // 2. If C has a [[BoundTargetFunction]] internal slot, then
    if (is<BoundFunction>(rhs_function)) {
        auto const& bound_target = static_cast<BoundFunction const&>(rhs_function);

        // a. Let BC be C.[[BoundTargetFunction]].
        // b. Return ? InstanceofOperator(O, BC).
        return instance_of(vm, lhs, Value(&bound_target.bound_target_function()));
    }

    // 3. If O is not an Object, return false.
    if (!lhs.is_object())
        return Value(false);

    auto* lhs_object = &lhs.as_object();

    // 4. Let P be ? Get(C, "prototype").
    auto rhs_prototype = TRY(rhs_function.get(vm.names.prototype));

    // 5. If P is not an Object, throw a TypeError exception.
    if (!rhs_prototype.is_object())
        return vm.throw_completion<TypeError>(ErrorType::InstanceOfOperatorBadPrototype, rhs.to_string_without_side_effects());

    // 6. Repeat,
    while (true) {
        // a. Set O to ? O.[[GetPrototypeOf]]().
        lhs_object = TRY(lhs_object->internal_get_prototype_of());

        // b. If O is null, return false.
        if (!lhs_object)
            return Value(false);

        // c. If SameValue(P, O) is true, return true.
        if (same_value(rhs_prototype, lhs_object))
            return Value(true);
    }
}

// 7.2.10 SameValue ( x, y ), https://tc39.es/ecma262/#sec-samevalue
bool same_value(Value lhs, Value rhs)
{
    // 1. If Type(x) is different from Type(y), return false.
    if (!same_type_for_equality(lhs, rhs))
        return false;

    // 2. If x is a Number, then
    if (lhs.is_number()) {
        // a. Return Number::sameValue(x, y).

        // 6.1.6.1.14 Number::sameValue ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-sameValue
        // 1. If x is NaN and y is NaN, return true.
        if (lhs.is_nan() && rhs.is_nan())
            return true;
        // 2. If x is +0𝔽 and y is -0𝔽, return false.
        if (lhs.is_positive_zero() && rhs.is_negative_zero())
            return false;
        // 3. If x is -0𝔽 and y is +0𝔽, return false.
        if (lhs.is_negative_zero() && rhs.is_positive_zero())
            return false;
        // 4. If x is the same Number value as y, return true.
        // 5. Return false.
        return lhs.as_double() == rhs.as_double();
    }

    // 3. Return SameValueNonNumber(x, y).
    return same_value_non_number(lhs, rhs);
}

// 7.2.11 SameValueZero ( x, y ), https://tc39.es/ecma262/#sec-samevaluezero
bool same_value_zero(Value lhs, Value rhs)
{
    // 1. If Type(x) is different from Type(y), return false.
    if (!same_type_for_equality(lhs, rhs))
        return false;

    // 2. If x is a Number, then
    if (lhs.is_number()) {
        // a. Return Number::sameValueZero(x, y).
        if (lhs.is_nan() && rhs.is_nan())
            return true;
        return lhs.as_double() == rhs.as_double();
    }

    // 3. Return SameValueNonNumber(x, y).
    return same_value_non_number(lhs, rhs);
}

// 7.2.12 SameValueNonNumber ( x, y ), https://tc39.es/ecma262/#sec-samevaluenonnumeric
bool same_value_non_number(Value lhs, Value rhs)
{
    // 1. Assert: Type(x) is the same as Type(y).
    VERIFY(same_type_for_equality(lhs, rhs));
    VERIFY(!lhs.is_number());

    // 2. If x is a BigInt, then
    if (lhs.is_bigint()) {
        // a. Return BigInt::equal(x, y).

        // 6.1.6.2.13 BigInt::equal ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-equal
        // 1. If ℝ(x) = ℝ(y), return true; otherwise return false.
        return lhs.as_bigint().big_integer() == rhs.as_bigint().big_integer();
    }

    // 5. If x is a String, then
    if (lhs.is_string()) {
        // a. If x and y are exactly the same sequence of code units (same length and same code units at corresponding indices), return true; otherwise, return false.
        return lhs.as_string().utf8_string_view() == rhs.as_string().utf8_string_view();
    }

    // 3. If x is undefined, return true.
    // 4. If x is null, return true.
    // 6. If x is a Boolean, then
    //    a. If x and y are both true or both false, return true; otherwise, return false.
    // 7. If x is a Symbol, then
    //    a. If x and y are both the same Symbol value, return true; otherwise, return false.
    // 8. If x and y are the same Object value, return true. Otherwise, return false.
    // NOTE: All the options above will have the exact same bit representation in Value, so we can directly compare the bits.
    return lhs.m_value.encoded == rhs.m_value.encoded;
}

// 7.2.15 IsStrictlyEqual ( x, y ), https://tc39.es/ecma262/#sec-isstrictlyequal
bool is_strictly_equal(Value lhs, Value rhs)
{
    // 1. If Type(x) is different from Type(y), return false.
    if (!same_type_for_equality(lhs, rhs))
        return false;

    // 2. If x is a Number, then
    if (lhs.is_number()) {
        // a. Return Number::equal(x, y).

        // 6.1.6.1.13 Number::equal ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-equal
        // 1. If x is NaN, return false.
        // 2. If y is NaN, return false.
        if (lhs.is_nan() || rhs.is_nan())
            return false;
        // 3. If x is the same Number value as y, return true.
        // 4. If x is +0𝔽 and y is -0𝔽, return true.
        // 5. If x is -0𝔽 and y is +0𝔽, return true.
        if (lhs.as_double() == rhs.as_double())
            return true;
        // 6. Return false.
        return false;
    }

    // 3. Return SameValueNonNumber(x, y).
    return same_value_non_number(lhs, rhs);
}

// 7.2.14 IsLooselyEqual ( x, y ), https://tc39.es/ecma262/#sec-islooselyequal
ThrowCompletionOr<bool> is_loosely_equal(VM& vm, Value lhs, Value rhs)
{
    // 1. If Type(x) is the same as Type(y), then
    if (same_type_for_equality(lhs, rhs)) {
        // a. Return IsStrictlyEqual(x, y).
        return is_strictly_equal(lhs, rhs);
    }

    // 2. If x is null and y is undefined, return true.
    // 3. If x is undefined and y is null, return true.
    if (lhs.is_nullish() && rhs.is_nullish())
        return true;

    // 4. NOTE: This step is replaced in section B.3.6.2.
    // B.3.6.2 Changes to IsLooselyEqual, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot-aec
    // 4. Perform the following steps:
    // a. If Type(x) is Object and x has an [[IsHTMLDDA]] internal slot and y is either null or undefined, return true.
    if (lhs.is_object() && lhs.as_object().is_htmldda() && rhs.is_nullish())
        return true;

    // b. If x is either null or undefined and Type(y) is Object and y has an [[IsHTMLDDA]] internal slot, return true.
    if (lhs.is_nullish() && rhs.is_object() && rhs.as_object().is_htmldda())
        return true;

    // == End of B.3.6.2 ==

    // 5. If Type(x) is Number and Type(y) is String, return ! IsLooselyEqual(x, ! ToNumber(y)).
    if (lhs.is_number() && rhs.is_string())
        return is_loosely_equal(vm, lhs, MUST(rhs.to_number(vm)));

    // 6. If Type(x) is String and Type(y) is Number, return ! IsLooselyEqual(! ToNumber(x), y).
    if (lhs.is_string() && rhs.is_number())
        return is_loosely_equal(vm, MUST(lhs.to_number(vm)), rhs);

    // 7. If Type(x) is BigInt and Type(y) is String, then
    if (lhs.is_bigint() && rhs.is_string()) {
        // a. Let n be StringToBigInt(y).
        auto bigint = string_to_bigint(vm, rhs.as_string().utf8_string_view());

        // b. If n is undefined, return false.
        if (!bigint.has_value())
            return false;

        // c. Return ! IsLooselyEqual(x, n).
        return is_loosely_equal(vm, lhs, *bigint);
    }

    // 8. If Type(x) is String and Type(y) is BigInt, return ! IsLooselyEqual(y, x).
    if (lhs.is_string() && rhs.is_bigint())
        return is_loosely_equal(vm, rhs, lhs);

    // 9. If Type(x) is Boolean, return ! IsLooselyEqual(! ToNumber(x), y).
    if (lhs.is_boolean())
        return is_loosely_equal(vm, MUST(lhs.to_number(vm)), rhs);

    // 10. If Type(y) is Boolean, return ! IsLooselyEqual(x, ! ToNumber(y)).
    if (rhs.is_boolean())
        return is_loosely_equal(vm, lhs, MUST(rhs.to_number(vm)));

    // 11. If Type(x) is either String, Number, BigInt, or Symbol and Type(y) is Object, return ! IsLooselyEqual(x, ? ToPrimitive(y)).
    if ((lhs.is_string() || lhs.is_number() || lhs.is_bigint() || lhs.is_symbol()) && rhs.is_object()) {
        auto rhs_primitive = TRY(rhs.to_primitive(vm));
        return is_loosely_equal(vm, lhs, rhs_primitive);
    }

    // 12. If Type(x) is Object and Type(y) is either String, Number, BigInt, or Symbol, return ! IsLooselyEqual(? ToPrimitive(x), y).
    if (lhs.is_object() && (rhs.is_string() || rhs.is_number() || rhs.is_bigint() || rhs.is_symbol())) {
        auto lhs_primitive = TRY(lhs.to_primitive(vm));
        return is_loosely_equal(vm, lhs_primitive, rhs);
    }

    // 13. If Type(x) is BigInt and Type(y) is Number, or if Type(x) is Number and Type(y) is BigInt, then
    if ((lhs.is_bigint() && rhs.is_number()) || (lhs.is_number() && rhs.is_bigint())) {
        // a. If x or y are any of NaN, +∞𝔽, or -∞𝔽, return false.
        if (lhs.is_nan() || lhs.is_infinity() || rhs.is_nan() || rhs.is_infinity())
            return false;

        // b. If ℝ(x) = ℝ(y), return true; otherwise return false.
        if ((lhs.is_number() && !lhs.is_integral_number()) || (rhs.is_number() && !rhs.is_integral_number()))
            return false;

        VERIFY(!lhs.is_nan() && !rhs.is_nan());

        auto& number_side = lhs.is_number() ? lhs : rhs;
        auto& bigint_side = lhs.is_number() ? rhs : lhs;

        return bigint_side.as_bigint().big_integer().compare_to_double(number_side.as_double()) == Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt;
    }

    // 14. Return false.
    return false;
}

// 7.2.13 IsLessThan ( x, y, LeftFirst ), https://tc39.es/ecma262/#sec-islessthan
ThrowCompletionOr<TriState> is_less_than(VM& vm, Value lhs, Value rhs, bool left_first)
{
    Value x_primitive;
    Value y_primitive;

    // 1. If the LeftFirst flag is true, then
    if (left_first) {
        // a. Let px be ? ToPrimitive(x, number).
        x_primitive = TRY(lhs.to_primitive(vm, Value::PreferredType::Number));

        // b. Let py be ? ToPrimitive(y, number).
        y_primitive = TRY(rhs.to_primitive(vm, Value::PreferredType::Number));
    } else {
        // a. NOTE: The order of evaluation needs to be reversed to preserve left to right evaluation.

        // b. Let py be ? ToPrimitive(y, number).
        y_primitive = TRY(lhs.to_primitive(vm, Value::PreferredType::Number));

        // c. Let px be ? ToPrimitive(x, number).
        x_primitive = TRY(rhs.to_primitive(vm, Value::PreferredType::Number));
    }

    // 3. If px is a String and py is a String, then
    if (x_primitive.is_string() && y_primitive.is_string()) {
        auto x_string = x_primitive.as_string().utf8_string_view();
        auto y_string = y_primitive.as_string().utf8_string_view();

        Utf8View x_code_points { x_string };
        Utf8View y_code_points { y_string };

        // a. Let lx be the length of px.
        // b. Let ly be the length of py.
        // c. For each integer i such that 0 ≤ i < min(lx, ly), in ascending order, do
        for (auto k = x_code_points.begin(), l = y_code_points.begin();
            k != x_code_points.end() && l != y_code_points.end();
            ++k, ++l) {
            // i. Let cx be the integer that is the numeric value of the code unit at index i within px.
            // ii. Let cy be the integer that is the numeric value of the code unit at index i within py.
            if (*k != *l) {
                // iii. If cx < cy, return true.
                if (*k < *l) {
                    return TriState::True;
                }
                // iv. If cx > cy, return false.
                else {
                    return TriState::False;
                }
            }
        }

        // d. If lx < ly, return true. Otherwise, return false.
        return x_code_points.length() < y_code_points.length()
            ? TriState::True
            : TriState::False;
    }

    // 4. Else,
    // a. If px is a BigInt and py is a String, then
    if (x_primitive.is_bigint() && y_primitive.is_string()) {
        // i. Let ny be StringToBigInt(py).
        auto y_bigint = string_to_bigint(vm, y_primitive.as_string().utf8_string_view());

        // ii. If ny is undefined, return undefined.
        if (!y_bigint.has_value())
            return TriState::Unknown;

        // iii. Return BigInt::lessThan(px, ny).
        if (x_primitive.as_bigint().big_integer() < (*y_bigint)->big_integer())
            return TriState::True;
        return TriState::False;
    }

    // b. If px is a String and py is a BigInt, then
    if (x_primitive.is_string() && y_primitive.is_bigint()) {
        // i. Let nx be StringToBigInt(px).
        auto x_bigint = string_to_bigint(vm, x_primitive.as_string().utf8_string_view());

        // ii. If nx is undefined, return undefined.
        if (!x_bigint.has_value())
            return TriState::Unknown;

        // iii. Return BigInt::lessThan(nx, py).
        if ((*x_bigint)->big_integer() < y_primitive.as_bigint().big_integer())
            return TriState::True;
        return TriState::False;
    }

    // c. NOTE: Because px and py are primitive values, evaluation order is not important.

    // d. Let nx be ? ToNumeric(px).
    auto x_numeric = TRY(x_primitive.to_numeric(vm));

    // e. Let ny be ? ToNumeric(py).
    auto y_numeric = TRY(y_primitive.to_numeric(vm));

    // h. If nx or ny is NaN, return undefined.
    if (x_numeric.is_nan() || y_numeric.is_nan())
        return TriState::Unknown;

    // i. If nx is -∞𝔽 or ny is +∞𝔽, return true.
    if (x_numeric.is_positive_infinity() || y_numeric.is_negative_infinity())
        return TriState::False;

    // j. If nx is +∞𝔽 or ny is -∞𝔽, return false.
    if (x_numeric.is_negative_infinity() || y_numeric.is_positive_infinity())
        return TriState::True;

    // f. If Type(nx) is the same as Type(ny), then

    // i. If nx is a Number, then
    if (x_numeric.is_number() && y_numeric.is_number()) {
        // 1. Return Number::lessThan(nx, ny).
        if (x_numeric.as_double() < y_numeric.as_double())
            return TriState::True;
        else
            return TriState::False;
    }
    // ii. Else,
    if (x_numeric.is_bigint() && y_numeric.is_bigint()) {
        // 1. Assert: nx is a BigInt.
        // 2. Return BigInt::lessThan(nx, ny).
        if (x_numeric.as_bigint().big_integer() < y_numeric.as_bigint().big_integer())
            return TriState::True;
        else
            return TriState::False;
    }

    // g. Assert: nx is a BigInt and ny is a Number, or nx is a Number and ny is a BigInt.
    VERIFY((x_numeric.is_number() && y_numeric.is_bigint()) || (x_numeric.is_bigint() && y_numeric.is_number()));

    // k. If ℝ(nx) < ℝ(ny), return true; otherwise return false.
    bool x_lower_than_y;
    VERIFY(!x_numeric.is_nan() && !y_numeric.is_nan());
    if (x_numeric.is_number()) {
        x_lower_than_y = y_numeric.as_bigint().big_integer().compare_to_double(x_numeric.as_double())
            == Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt;
    } else {
        x_lower_than_y = x_numeric.as_bigint().big_integer().compare_to_double(y_numeric.as_double())
            == Crypto::UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt;
    }
    if (x_lower_than_y)
        return TriState::True;
    else
        return TriState::False;
}

// 7.3.21 Invoke ( V, P [ , argumentsList ] ), https://tc39.es/ecma262/#sec-invoke
ThrowCompletionOr<Value> Value::invoke_internal(VM& vm, PropertyKey const& property_key, Optional<GC::RootVector<Value>> arguments)
{
    // 1. If argumentsList is not present, set argumentsList to a new empty List.

    // 2. Let func be ? GetV(V, P).
    auto function = TRY(get(vm, property_key));

    // 3. Return ? Call(func, V, argumentsList).
    ReadonlySpan<Value> argument_list;
    if (arguments.has_value())
        argument_list = arguments.value().span();
    return call(vm, function, *this, argument_list);
}

}
