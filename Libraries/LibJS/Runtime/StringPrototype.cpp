/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Function.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/Collator.h>
#include <LibJS/Runtime/Intl/CollatorCompareFunction.h>
#include <LibJS/Runtime/Intl/CollatorConstructor.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/StringIterator.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/Runtime/Utf16String.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/Normalize.h>
#include <string.h>

namespace JS {

GC_DEFINE_ALLOCATOR(StringPrototype);

static ThrowCompletionOr<String> utf8_string_from(VM& vm)
{
    auto this_value = TRY(require_object_coercible(vm, vm.this_value()));
    return TRY(this_value.to_string(vm));
}

static ThrowCompletionOr<Utf16String> utf16_string_from(VM& vm)
{
    auto this_value = TRY(require_object_coercible(vm, vm.this_value()));
    return TRY(this_value.to_utf16_string(vm));
}

// 22.1.3.21.1 SplitMatch ( S, q, R ), https://tc39.es/ecma262/#sec-splitmatch
// FIXME: This no longer exists in the spec!
static Optional<size_t> split_match(Utf16View const& haystack, size_t start, Utf16View const& needle)
{
    auto r = needle.length_in_code_units();
    auto s = haystack.length_in_code_units();
    if (start + r > s)
        return {};
    for (size_t i = 0; i < r; ++i) {
        if (haystack.code_unit_at(start + i) != needle.code_unit_at(i))
            return {};
    }
    return start + r;
}

// 6.1.4.1 StringIndexOf ( string, searchValue, fromIndex ), https://tc39.es/ecma262/#sec-stringindexof
Optional<size_t> string_index_of(Utf16View const& string, Utf16View const& search_value, size_t from_index)
{
    // 1. Let len be the length of string.
    size_t string_length = string.length_in_code_units();

    // 3. Let searchLen be the length of searchValue.
    size_t search_length = search_value.length_in_code_units();

    // 2. If searchValue is the empty String and fromIndex ≤ len, return fromIndex.
    if ((search_length == 0) && (from_index <= string_length))
        return from_index;

    // OPTIMIZATION: If the needle is longer than the haystack, don't bother searching :^)
    if (search_length > string_length)
        return {};

    // 4. For each integer i such that fromIndex ≤ i ≤ len - searchLen, in ascending order, do
    for (size_t i = from_index; i <= string_length - search_length; ++i) {
        // a. Let candidate be the substring of string from i to i + searchLen.
        auto candidate = string.substring_view(i, search_length);

        // b. If candidate is searchValue, return i.
        if (candidate == search_value)
            return i;
    }

    // 5. Return -1.
    return {};
}

// 7.2.9 Static Semantics: IsStringWellFormedUnicode ( string )
static bool is_string_well_formed_unicode(Utf16View string)
{
    // OPTIMIZATION: simdutf can do this much faster.
    return string.validate();
}

// 11.1.4 CodePointAt ( string, position ), https://tc39.es/ecma262/#sec-codepointat
CodePoint code_point_at(Utf16View const& string, size_t position)
{
    // 1. Let size be the length of string.
    // 2. Assert: position ≥ 0 and position < size.
    VERIFY(position < string.length_in_code_units());

    // 3. Let first be the code unit at index position within string.
    auto first = string.code_unit_at(position);

    // 4. Let cp be the code point whose numeric value is that of first.
    auto code_point = static_cast<u32>(first);

    // 5. If first is not a leading surrogate or trailing surrogate, then
    if (!is_unicode_surrogate(first)) {
        // a. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 1, [[IsUnpairedSurrogate]]: false }.
        return { false, code_point, 1 };
    }

    // 6. If first is a trailing surrogate or position + 1 = size, then
    if (Utf16View::is_low_surrogate(first) || (position + 1 == string.length_in_code_units())) {
        // a. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 1, [[IsUnpairedSurrogate]]: true }.
        return { true, code_point, 1 };
    }

    // 7. Let second be the code unit at index position + 1 within string.
    auto second = string.code_unit_at(position + 1);

    // 8. If second is not a trailing surrogate, then
    if (!Utf16View::is_low_surrogate(second)) {
        // a. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 1, [[IsUnpairedSurrogate]]: true }.
        return { true, code_point, 1 };
    }

    // 9. Set cp to UTF16SurrogatePairToCodePoint(first, second).
    code_point = Utf16View::decode_surrogate_pair(first, second);

    // 10. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 2, [[IsUnpairedSurrogate]]: false }.
    return { false, code_point, 2 };
}

StringPrototype::StringPrototype(Realm& realm)
    : StringObject(*PrimitiveString::create(realm.vm(), String {}), realm.intrinsics().object_prototype())
{
}

void StringPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    // 22.1.3 Properties of the String Prototype Object, https://tc39.es/ecma262/#sec-properties-of-the-string-prototype-object
    define_native_function(realm, vm.names.at, at, 1, attr);
    define_native_function(realm, vm.names.charAt, char_at, 1, attr);
    define_native_function(realm, vm.names.charCodeAt, char_code_at, 1, attr);
    define_native_function(realm, vm.names.codePointAt, code_point_at, 1, attr);
    define_native_function(realm, vm.names.concat, concat, 1, attr);
    define_native_function(realm, vm.names.endsWith, ends_with, 1, attr);
    define_native_function(realm, vm.names.includes, includes, 1, attr);
    define_native_function(realm, vm.names.indexOf, index_of, 1, attr);
    define_native_function(realm, vm.names.isWellFormed, is_well_formed, 0, attr);
    define_native_function(realm, vm.names.lastIndexOf, last_index_of, 1, attr);
    define_native_function(realm, vm.names.localeCompare, locale_compare, 1, attr);
    define_native_function(realm, vm.names.match, match, 1, attr);
    define_native_function(realm, vm.names.matchAll, match_all, 1, attr);
    define_native_function(realm, vm.names.normalize, normalize, 0, attr);
    define_native_function(realm, vm.names.padEnd, pad_end, 1, attr);
    define_native_function(realm, vm.names.padStart, pad_start, 1, attr);
    define_native_function(realm, vm.names.repeat, repeat, 1, attr);
    define_native_function(realm, vm.names.replace, replace, 2, attr);
    define_native_function(realm, vm.names.replaceAll, replace_all, 2, attr);
    define_native_function(realm, vm.names.search, search, 1, attr);
    define_native_function(realm, vm.names.slice, slice, 2, attr);
    define_native_function(realm, vm.names.split, split, 2, attr);
    define_native_function(realm, vm.names.startsWith, starts_with, 1, attr);
    define_native_function(realm, vm.names.substring, substring, 2, attr);
    define_native_function(realm, vm.names.toLocaleLowerCase, to_locale_lowercase, 0, attr);
    define_native_function(realm, vm.names.toLocaleUpperCase, to_locale_uppercase, 0, attr);
    define_native_function(realm, vm.names.toLowerCase, to_lowercase, 0, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toUpperCase, to_uppercase, 0, attr);
    define_native_function(realm, vm.names.toWellFormed, to_well_formed, 0, attr);
    define_native_function(realm, vm.names.trim, trim, 0, attr);
    define_native_function(realm, vm.names.trimEnd, trim_end, 0, attr);
    define_native_function(realm, vm.names.trimStart, trim_start, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
    define_native_function(realm, vm.well_known_symbol_iterator(), symbol_iterator, 0, attr);

    // B.2.2 Additional Properties of the String.prototype Object, https://tc39.es/ecma262/#sec-additional-properties-of-the-string.prototype-object
    define_native_function(realm, vm.names.substr, substr, 2, attr);
    define_native_function(realm, vm.names.anchor, anchor, 1, attr);
    define_native_function(realm, vm.names.big, big, 0, attr);
    define_native_function(realm, vm.names.blink, blink, 0, attr);
    define_native_function(realm, vm.names.bold, bold, 0, attr);
    define_native_function(realm, vm.names.fixed, fixed, 0, attr);
    define_native_function(realm, vm.names.fontcolor, fontcolor, 1, attr);
    define_native_function(realm, vm.names.fontsize, fontsize, 1, attr);
    define_native_function(realm, vm.names.italics, italics, 0, attr);
    define_native_function(realm, vm.names.link, link, 1, attr);
    define_native_function(realm, vm.names.small, small, 0, attr);
    define_native_function(realm, vm.names.strike, strike, 0, attr);
    define_native_function(realm, vm.names.sub, sub, 0, attr);
    define_native_function(realm, vm.names.sup, sup, 0, attr);
    define_direct_property(vm.names.trimLeft, get_without_side_effects(vm.names.trimStart), attr);
    define_direct_property(vm.names.trimRight, get_without_side_effects(vm.names.trimEnd), attr);
}

// thisStringValue ( value ), https://tc39.es/ecma262/#thisstringvalue
static ThrowCompletionOr<GC::Ref<PrimitiveString>> this_string_value(VM& vm, Value value)
{
    // 1. If value is a String, return value.
    if (value.is_string())
        return value.as_string();

    // 2. If value is an Object and value has a [[StringData]] internal slot, then
    if (value.is_object() && is<StringObject>(value.as_object())) {
        // a. Let s be value.[[StringData]].
        // b. Assert: s is a String.
        // c. Return s.
        return static_cast<StringObject&>(value.as_object()).primitive_string();
    }

    // 3. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "String");
}

// 22.1.3.1 String.prototype.at ( index ), https://tc39.es/ecma262/#sec-string.prototype.at
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::at)
{
    // 1. Let O be ? ToObject(this value).
    auto string = TRY(utf16_string_from(vm));
    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = string.length_in_code_units();

    // 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
    auto relative_index = TRY(vm.argument(0).to_integer_or_infinity(vm));
    if (Value(relative_index).is_infinity())
        return js_undefined();

    Checked<size_t> index { 0 };
    // 4. If relativeIndex ≥ 0, then
    if (relative_index >= 0) {
        // a. Let k be relativeIndex.
        index += relative_index;
    }
    // 5. Else,
    else {
        // a. Let k be len + relativeIndex.
        index += length;
        index -= -relative_index;
    }
    // 6. If k < 0 or k ≥ len, return undefined.
    if (index.has_overflow() || index.value() >= length)
        return js_undefined();

    // 7. Return ? Get(O, ! ToString(𝔽(k))).
    return PrimitiveString::create(vm, Utf16String::create(string.substring_view(index.value(), 1)));
}

// 22.1.3.2 String.prototype.charAt ( pos ), https://tc39.es/ecma262/#sec-string.prototype.charat
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::char_at)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let position be ? ToIntegerOrInfinity(pos).
    auto position = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 4. Let size be the length of S.
    // 5. If position < 0 or position ≥ size, return the empty String.
    if (position < 0 || position >= string.length_in_code_units())
        return PrimitiveString::create(vm, String {});

    // 6. Return the substring of S from position to position + 1.
    return PrimitiveString::create(vm, Utf16String::create(string.substring_view(position, 1)));
}

// 22.1.3.3 String.prototype.charCodeAt ( pos ), https://tc39.es/ecma262/#sec-string.prototype.charcodeat
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::char_code_at)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let position be ? ToIntegerOrInfinity(pos).
    auto position = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 4. Let size be the length of S.
    // 5. If position < 0 or position ≥ size, return NaN.
    if (position < 0 || position >= string.length_in_code_units())
        return js_nan();

    // 6. Return the Number value for the numeric value of the code unit at index position within the String S.
    return Value(string.code_unit_at(position));
}

// 22.1.3.4 String.prototype.codePointAt ( pos ), https://tc39.es/ecma262/#sec-string.prototype.codepointat
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::code_point_at)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let position be ? ToIntegerOrInfinity(pos).
    auto position = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 4. Let size be the length of S.
    // 5. If position < 0 or position ≥ size, return undefined.
    if (position < 0 || position >= string.length_in_code_units())
        return js_undefined();

    // 6. Let cp be CodePointAt(S, position).
    auto code_point = JS::code_point_at(string.view(), position);

    // 7. Return 𝔽(cp.[[CodePoint]]).
    return Value(code_point.code_point);
}

// 22.1.3.5 String.prototype.concat ( ...args ), https://tc39.es/ecma262/#sec-string.prototype.concat
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::concat)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    auto object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. Let S be ? ToString(O).
    auto string = TRY(object.to_primitive_string(vm));

    // 3. Let R be S.
    auto result = string;

    // 4. For each element next of args, do
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        // a. Let nextString be ? ToString(next).
        auto next_string = TRY(vm.argument(i).to_primitive_string(vm));

        // b. Set R to the string-concatenation of R and nextString.
        result = PrimitiveString::create(vm, *result, *next_string);
    }

    // 5. Return R.
    return result;
}

// 22.1.3.7 String.prototype.endsWith ( searchString [ , endPosition ] ), https://tc39.es/ecma262/#sec-string.prototype.endswith
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::ends_with)
{
    auto search_string_value = vm.argument(0);
    auto end_position = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // Let isRegExp be ? IsRegExp(searchString).
    bool is_regexp = TRY(search_string_value.is_regexp(vm));

    // 4. If isRegExp is true, throw a TypeError exception.
    if (is_regexp)
        return vm.throw_completion<TypeError>(ErrorType::IsNotA, "searchString", "string, but a regular expression");

    // 5. Let searchStr be ? ToString(searchString).
    auto search_string = TRY(search_string_value.to_utf16_string(vm));

    // 6. Let len be the length of S.
    auto string_length = string.length_in_code_units();

    // 7. If endPosition is undefined, let pos be len; else let pos be ? ToIntegerOrInfinity(endPosition).
    size_t end = string_length;
    if (!end_position.is_undefined()) {
        auto position = TRY(end_position.to_integer_or_infinity(vm));

        // 8. Let end be the result of clamping pos between 0 and len.
        end = clamp(position, static_cast<double>(0), static_cast<double>(string_length));
    }

    // 9. Let searchLength be the length of searchStr.
    auto search_length = search_string.length_in_code_units();

    // 10. If searchLength = 0, return true.
    if (search_length == 0)
        return Value(true);

    // 12. If start < 0, return false.
    if (search_length > end)
        return Value(false);

    // 11. Let start be end - searchLength.
    size_t start = end - search_length;

    // 13. Let substring be the substring of S from start to end.
    auto substring_view = string.substring_view(start, end - start);

    // 14. If substring is searchStr, return true.
    // 15. Return false.
    return Value(substring_view == search_string.view());
}

// 22.1.3.8 String.prototype.includes ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.includes
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::includes)
{
    auto search_string_value = vm.argument(0);
    auto position = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let isRegExp be ? IsRegExp(searchString).
    bool is_regexp = TRY(search_string_value.is_regexp(vm));

    // 4. If isRegExp is true, throw a TypeError exception.
    if (is_regexp)
        return vm.throw_completion<TypeError>(ErrorType::IsNotA, "searchString", "string, but a regular expression");

    // 5. Let searchStr be ? ToString(searchString).
    auto search_string = TRY(search_string_value.to_utf16_string(vm));

    size_t start = 0;
    if (!position.is_undefined()) {
        // 6. Let pos be ? ToIntegerOrInfinity(position).
        // 7. Assert: If position is undefined, then pos is 0.
        auto pos = TRY(position.to_integer_or_infinity(vm));

        // 8. Let len be the length of S.
        // 9. Let start be the result of clamping pos between 0 and len.
        start = clamp(pos, static_cast<double>(0), static_cast<double>(string.length_in_code_units()));
    }

    // 10. Let index be StringIndexOf(S, searchStr, start).
    auto index = string_index_of(string.view(), search_string.view(), start);

    // 11. If index ≠ -1, return true.
    // 12. Return false.
    return Value(index.has_value());
}

// 22.1.3.9 String.prototype.indexOf ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.indexof
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::index_of)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let searchStr be ? ToString(searchString).
    auto search_string = TRY(vm.argument(0).to_utf16_string(vm));

    auto utf16_string_view = string.view();
    auto utf16_search_view = search_string.view();

    size_t start = 0;
    if (vm.argument_count() > 1) {
        // 4. Let pos be ? ToIntegerOrInfinity(position).
        // 5. Assert: If position is undefined, then pos is 0.
        auto position = TRY(vm.argument(1).to_integer_or_infinity(vm));

        // 6. Let len be the length of S.
        // 7. Let start be the result of clamping pos between 0 and len.
        start = clamp(position, static_cast<double>(0), static_cast<double>(utf16_string_view.length_in_code_units()));
    }

    // 8. Return 𝔽(StringIndexOf(S, searchStr, start)).
    auto index = string_index_of(utf16_string_view, utf16_search_view, start);
    return index.has_value() ? Value(*index) : Value(-1);
}

// 22.1.3.10 String.prototype.isWellFormed ( ), https://tc39.es/ecma262/#sec-string.prototype.iswellformed
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::is_well_formed)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Return IsStringWellFormedUnicode(S).
    return is_string_well_formed_unicode(string.view());
}

// 22.1.3.11 String.prototype.lastIndexOf ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.lastindexof
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::last_index_of)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let searchStr be ? ToString(searchString).
    auto search_string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Let numPos be ? ToNumber(position).
    // 5. Assert: If position is undefined, then numPos is NaN.
    auto position = TRY(vm.argument(1).to_number(vm));

    // 6. If numPos is NaN, let pos be +∞; otherwise, let pos be ! ToIntegerOrInfinity(numPos).
    double pos = position.is_nan() ? static_cast<double>(INFINITY) : MUST(position.to_integer_or_infinity(vm));

    // 7. Let len be the length of S.
    auto string_length = string.length_in_code_units();

    // 8. Let searchLen be the length of searchStr.
    auto search_length = search_string.length_in_code_units();

    // 9. Let start be the result of clamping pos between 0 and len - searchLen.
    size_t start = clamp(pos, static_cast<double>(0), static_cast<double>(string_length));
    Optional<size_t> last_index;

    // 10. If searchStr is the empty String, return 𝔽(start).
    // 11. For each integer i such that 0 ≤ i ≤ start, in descending order, do
    for (size_t k = 0; (k <= start) && (k + search_length <= string_length); ++k) {
        bool is_match = true;

        // a. Let candidate be the substring of S from i to i + searchLen.
        for (size_t j = 0; j < search_length; ++j) {
            if (string.code_unit_at(k + j) != search_string.code_unit_at(j)) {
                is_match = false;
                break;
            }
        }

        // b. If candidate is searchStr, return 𝔽(i).
        if (is_match)
            last_index = k;
    }

    // 12. Return -1𝔽.
    return last_index.has_value() ? Value(*last_index) : Value(-1);
}

// 22.1.3.12 String.prototype.localeCompare ( that [ , reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-string.prototype.localecompare
// 20.1.1 String.prototype.localeCompare ( that [ , locales [ , options ] ] ), https://tc39.es/ecma402/#sup-String.prototype.localeCompare
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::locale_compare)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. Let S be ? ToString(O).
    auto string = TRY(object.to_string(vm));

    // 3. Let thatValue be ? ToString(that).
    auto that_value = TRY(vm.argument(0).to_string(vm));

    // 4. Let collator be ? Construct(%Collator%, « locales, options »).
    auto locales = vm.argument(1);
    auto options = vm.argument(2);

    // OPTIMIZATION: If both locales and options are undefined, we can use a cached default-constructed Collator.
    GC::Ptr<Object> collator;
    if (locales.is_undefined() && options.is_undefined())
        collator = realm.intrinsics().default_collator();
    else
        collator = TRY(construct(vm, realm.intrinsics().intl_collator_constructor(), locales, options));

    // 5. Return CompareStrings(collator, S, thatValue).
    return Intl::compare_strings(static_cast<Intl::Collator const&>(*collator), string, that_value);
}

// 22.1.3.13 String.prototype.match ( regexp ), https://tc39.es/ecma262/#sec-string.prototype.match
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::match)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    auto this_object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. If regexp is neither undefined nor null, then
    auto regexp = vm.argument(0);
    if (!regexp.is_nullish()) {
        // a. Let matcher be ? GetMethod(regexp, @@match).
        auto matcher = TRY(regexp.get_method(vm, vm.well_known_symbol_match()));

        // b. If matcher is not undefined, then
        if (matcher) {
            // i. Return ? Call(matcher, regexp, « O »).
            return TRY(call(vm, *matcher, regexp, this_object));
        }
    }

    // 3. Let S be ? ToString(O).
    auto string = TRY(this_object.to_utf16_string(vm));

    // 4. Let rx be ? RegExpCreate(regexp, undefined).
    auto rx = TRY(regexp_create(vm, regexp, js_undefined()));

    // 5. Return ? Invoke(rx, @@match, « S »).
    return TRY(Value(rx).invoke(vm, vm.well_known_symbol_match(), PrimitiveString::create(vm, move(string))));
}

// 22.1.3.14 String.prototype.matchAll ( regexp ), https://tc39.es/ecma262/#sec-string.prototype.matchall
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::match_all)
{
    auto regexp = vm.argument(0);

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto this_object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. If regexp is neither undefined nor null, then
    if (!regexp.is_nullish()) {
        // a. Let isRegExp be ? IsRegExp(regexp).
        auto is_regexp = TRY(regexp.is_regexp(vm));

        // b. If isRegExp is true, then
        if (is_regexp) {
            // i. Let flags be ? Get(regexp, "flags").
            auto flags = TRY(regexp.as_object().get(vm.names.flags));

            // ii. Perform ? RequireObjectCoercible(flags).
            auto flags_object = TRY(require_object_coercible(vm, flags));

            // iii. If ? ToString(flags) does not contain "g", throw a TypeError exception.
            auto flags_string = TRY(flags_object.to_string(vm));
            if (!flags_string.contains('g'))
                return vm.throw_completion<TypeError>(ErrorType::StringNonGlobalRegExp);
        }

        // c. Let matcher be ? GetMethod(regexp, @@matchAll).
        auto matcher = TRY(regexp.get_method(vm, vm.well_known_symbol_match_all()));

        // d. If matcher is not undefined, then
        if (matcher) {
            // i. Return ? Call(matcher, regexp, « O »).
            return TRY(call(vm, *matcher, regexp, this_object));
        }
    }

    // 3. Let S be ? ToString(O).
    auto string = TRY(this_object.to_utf16_string(vm));

    // 4. Let rx be ? RegExpCreate(regexp, "g").
    auto rx = TRY(regexp_create(vm, regexp, PrimitiveString::create(vm, "g"_string)));

    // 5. Return ? Invoke(rx, @@matchAll, « S »).
    return TRY(Value(rx).invoke(vm, vm.well_known_symbol_match_all(), PrimitiveString::create(vm, move(string))));
}

// 22.1.3.15 String.prototype.normalize ( [ form ] ), https://tc39.es/ecma262/#sec-string.prototype.normalize
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::normalize)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf8_string_from(vm));

    String form;

    // 3. If form is undefined, let f be "NFC".
    if (auto form_value = vm.argument(0); form_value.is_undefined()) {
        form = "NFC"_string;
    }
    // 4. Else, let f be ? ToString(form).
    else {
        form = TRY(form_value.to_string(vm));
    }

    // 5. If f is not one of "NFC", "NFD", "NFKC", or "NFKD", throw a RangeError exception.
    if (!form.is_one_of("NFC"sv, "NFD"sv, "NFKC"sv, "NFKD"sv))
        return vm.throw_completion<RangeError>(ErrorType::InvalidNormalizationForm, form);

    // 6. Let ns be the String value that is the result of normalizing S into the normalization form named by f as specified in https://unicode.org/reports/tr15/.
    auto unicode_form = Unicode::normalization_form_from_string(form);
    auto ns = Unicode::normalize(string, unicode_form);

    // 7. Return ns.
    return PrimitiveString::create(vm, move(ns));
}

enum class PadPlacement {
    Start,
    End,
};

// 22.1.3.17.1 StringPad ( O, maxLength, fillString, placement ), https://tc39.es/ecma262/#sec-stringpad
static ThrowCompletionOr<Value> pad_string(VM& vm, Utf16String string, Value max_length, Value fill_string, PadPlacement placement)
{
    // 1. Let S be ? ToString(O).

    // 2. Let intMaxLength be ℝ(? ToLength(maxLength)).
    auto int_max_length = TRY(max_length.to_length(vm));

    // 3. Let stringLength be the length of S.
    auto string_length = string.length_in_code_units();

    // 4. If intMaxLength ≤ stringLength, return S.
    if (int_max_length <= string_length)
        return PrimitiveString::create(vm, move(string));

    // 5. If fillString is undefined, let filler be the String value consisting solely of the code unit 0x0020 (SPACE).
    auto filler = Utf16String::create(Utf16Data { 0x20 });
    if (!fill_string.is_undefined()) {
        // 6. Else, let filler be ? ToString(fillString).
        filler = TRY(fill_string.to_utf16_string(vm));

        // 7. If filler is the empty String, return S.
        if (filler.is_empty())
            return PrimitiveString::create(vm, move(string));
    }

    // 8. Let fillLen be intMaxLength - stringLength.
    auto fill_length = int_max_length - string_length;

    StringBuilder truncated_string_filler_builder;
    auto fill_code_units = filler.length_in_code_units();
    for (size_t i = 0; i < fill_length / fill_code_units; ++i)
        truncated_string_filler_builder.append(filler.view());

    // 9. Let truncatedStringFiller be the String value consisting of repeated concatenations of filler truncated to length fillLen.
    truncated_string_filler_builder.append(filler.substring_view(0, fill_length % fill_code_units));
    auto truncated_string_filler = MUST(truncated_string_filler_builder.to_string());

    // 10. If placement is start, return the string-concatenation of truncatedStringFiller and S.
    // 11. Else, return the string-concatenation of S and truncatedStringFiller.
    auto formatted = placement == PadPlacement::Start
        ? MUST(String::formatted("{}{}", truncated_string_filler, string.view()))
        : MUST(String::formatted("{}{}", string.view(), truncated_string_filler));
    return PrimitiveString::create(vm, move(formatted));
}

// 22.1.3.16 String.prototype.padEnd ( maxLength [ , fillString ] ), https://tc39.es/ecma262/#sec-string.prototype.padend
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::pad_end)
{
    auto max_length = vm.argument(0);
    auto fill_string = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto string = TRY(utf16_string_from(vm));

    // 2. Return ? StringPad(O, maxLength, fillString, end).
    return pad_string(vm, move(string), max_length, fill_string, PadPlacement::End);
}

// 22.1.3.17 String.prototype.padStart ( maxLength [ , fillString ] ), https://tc39.es/ecma262/#sec-string.prototype.padstart
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::pad_start)
{
    auto max_length = vm.argument(0);
    auto fill_string = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto string = TRY(utf16_string_from(vm));

    // 2. Return ? StringPad(O, maxLength, fillString, start).
    return pad_string(vm, move(string), max_length, fill_string, PadPlacement::Start);
}

// 22.1.3.18 String.prototype.repeat ( count ), https://tc39.es/ecma262/#sec-string.prototype.repeat
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::repeat)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf8_string_from(vm));

    // 3. Let n be ? ToIntegerOrInfinity(count).
    auto n = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 4. If n < 0 or n = +∞, throw a RangeError exception.
    if (n < 0)
        return vm.throw_completion<RangeError>(ErrorType::StringRepeatCountMustBe, "positive");
    if (Value(n).is_positive_infinity())
        return vm.throw_completion<RangeError>(ErrorType::StringRepeatCountMustBe, "finite");

    // 5. If n = 0, return the empty String.
    if (n == 0)
        return PrimitiveString::create(vm, String {});

    // OPTIMIZATION: If the string is empty, the result will be empty as well.
    if (string.is_empty())
        return PrimitiveString::create(vm, String {});

    auto repeated = String::repeated(string, n);
    if (repeated.is_error())
        return vm.throw_completion<RangeError>(ErrorType::StringRepeatCountMustNotOverflow);

    // 6. Return the String value that is made from n copies of S appended together.
    return PrimitiveString::create(vm, repeated.release_value());
}

// 22.1.3.19 String.prototype.replace ( searchValue, replaceValue ), https://tc39.es/ecma262/#sec-string.prototype.replace
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::replace)
{
    auto search_value = vm.argument(0);
    auto replace_value = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto this_object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. If searchValue is neither undefined nor null, then
    if (!search_value.is_nullish()) {
        // a. Let replacer be ? GetMethod(searchValue, @@replace).
        auto replacer = TRY(search_value.get_method(vm, vm.well_known_symbol_replace()));

        // b. If replacer is not undefined, then
        if (replacer) {
            // i. Return ? Call(replacer, searchValue, « O, replaceValue »).
            return TRY(call(vm, *replacer, search_value, this_object, replace_value));
        }
    }

    // 3. Let string be ? ToString(O).
    auto string = TRY(this_object.to_utf16_string(vm));

    // 4. Let searchString be ? ToString(searchValue).
    auto search_string = TRY(search_value.to_utf16_string(vm));

    // 5. Let functionalReplace be IsCallable(replaceValue).
    // 6. If functionalReplace is false, then
    if (!replace_value.is_function()) {
        // a. Set replaceValue to ? ToString(replaceValue).
        auto replace_string = TRY(replace_value.to_utf16_string(vm));
        replace_value = PrimitiveString::create(vm, move(replace_string));
    }

    // 7. Let searchLength be the length of searchString.
    auto search_length = search_string.length_in_code_units();

    // 8. Let position be StringIndexOf(string, searchString, 0).
    auto position = string_index_of(string.view(), search_string.view(), 0);

    // 9. If position = -1, return string.
    if (!position.has_value())
        return PrimitiveString::create(vm, move(string));

    // 10. Let preceding be the substring of string from 0 to position.
    auto preceding = string.substring_view(0, *position);
    String replacement;

    // 11. Let following be the substring of string from position + searchLength.
    auto following = string.substring_view(*position + search_length);

    // 12. If functionalReplace is true, then
    if (replace_value.is_function()) {
        // a. Let replacement be ? ToString(? Call(replaceValue, undefined, « searchString, 𝔽(position), string »)).
        auto result = TRY(call(vm, replace_value.as_function(), js_undefined(), PrimitiveString::create(vm, search_string), Value(*position), PrimitiveString::create(vm, string)));
        replacement = TRY(result.to_string(vm));
    }
    // 13. Else,
    else {
        // a. Assert: replaceValue is a String.
        VERIFY(replace_value.is_string());

        // b. Let captures be a new empty List.
        Span<Value> captures;

        // c. Let replacement be ! GetSubstitution(searchString, string, position, captures, undefined, replaceValue).
        replacement = TRY(get_substitution(vm, search_string.view(), string.view(), *position, captures, js_undefined(), replace_value));
    }

    // 14. Return the string-concatenation of preceding, replacement, and following.
    StringBuilder builder;
    builder.append(preceding);
    builder.append(replacement);
    builder.append(following);

    return PrimitiveString::create(vm, MUST(builder.to_string()));
}

// 22.1.3.20 String.prototype.replaceAll ( searchValue, replaceValue ), https://tc39.es/ecma262/#sec-string.prototype.replaceall
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::replace_all)
{
    auto search_value = vm.argument(0);
    auto replace_value = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto this_object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. If searchValue is neither undefined nor null, then
    if (!search_value.is_nullish()) {
        // a. Let isRegExp be ? IsRegExp(searchValue).
        bool is_regexp = TRY(search_value.is_regexp(vm));

        // b. If isRegExp is true, then
        if (is_regexp) {
            // i. Let flags be ? Get(searchValue, "flags").
            auto flags = TRY(search_value.as_object().get(vm.names.flags));

            // ii. Perform ? RequireObjectCoercible(flags).
            auto flags_object = TRY(require_object_coercible(vm, flags));

            // iii. If ? ToString(flags) does not contain "g", throw a TypeError exception.
            if (!TRY(flags_object.to_string(vm)).contains('g'))
                return vm.throw_completion<TypeError>(ErrorType::StringNonGlobalRegExp);
        }

        // c. Let replacer be ? GetMethod(searchValue, @@replace).
        auto replacer = TRY(search_value.get_method(vm, vm.well_known_symbol_replace()));

        // d. If replacer is not undefined, then
        if (replacer) {
            // i. Return ? Call(replacer, searchValue, « O, replaceValue »).
            return TRY(call(vm, *replacer, search_value, this_object, replace_value));
        }
    }

    // 3. Let string be ? ToString(O).
    auto string = TRY(this_object.to_utf16_string(vm));

    // 4. Let searchString be ? ToString(searchValue).
    auto search_string = TRY(search_value.to_utf16_string(vm));

    // 5. Let functionalReplace be IsCallable(replaceValue).
    // 6. If functionalReplace is false, then
    if (!replace_value.is_function()) {
        // a. Set replaceValue to ? ToString(replaceValue).
        auto replace_string = TRY(replace_value.to_utf16_string(vm));
        replace_value = PrimitiveString::create(vm, move(replace_string));
    }

    // 7. Let searchLength be the length of searchString.
    auto search_length = search_string.length_in_code_units();

    // 8. Let advanceBy be max(1, searchLength).
    size_t advance_by = max(1u, search_length);

    // 9. Let matchPositions be a new empty List.
    Vector<size_t> match_positions;

    // 10. Let position be StringIndexOf(string, searchString, 0).
    auto position = string_index_of(string.view(), search_string.view(), 0);

    // 11. Repeat, while position ≠ -1,
    while (position.has_value()) {
        // a. Append position to matchPositions.
        match_positions.append(*position);

        // b. Set position to StringIndexOf(string, searchString, position + advanceBy).
        position = string_index_of(string.view(), search_string.view(), *position + advance_by);
    }

    // 12. Let endOfLastMatch be 0.
    size_t end_of_last_match = 0;

    // 13. Let result be the empty String.
    StringBuilder result;

    // 14. For each element p of matchPositions, do
    for (auto position : match_positions) {
        // a. Let preserved be the substring of string from endOfLastMatch to p.
        auto preserved = string.substring_view(end_of_last_match, position - end_of_last_match);
        String replacement;

        // b. If functionalReplace is true, then
        if (replace_value.is_function()) {
            // i. Let replacement be ? ToString(? Call(replaceValue, undefined, « searchString, 𝔽(p), string »)).
            replacement = TRY(TRY(call(vm, replace_value.as_function(), js_undefined(), PrimitiveString::create(vm, search_string), Value(position), PrimitiveString::create(vm, string))).to_string(vm));
        }
        // c. Else,
        else {
            // i. Assert: replaceValue is a String.
            // ii. Let captures be a new empty List.
            // iii. Let replacement be ! GetSubstitution(searchString, string, p, captures, undefined, replaceValue).
            replacement = TRY(get_substitution(vm, search_string.view(), string.view(), position, {}, js_undefined(), replace_value));
        }

        // d. Set result to the string-concatenation of result, preserved, and replacement.
        result.append(preserved);
        result.append(replacement);

        // e. Set endOfLastMatch to p + searchLength.
        end_of_last_match = position + search_length;
    }

    auto string_length = string.length_in_code_units();

    // 15. If endOfLastMatch < the length of string, then
    if (end_of_last_match < string_length) {
        // a. Set result to the string-concatenation of result and the substring of string from endOfLastMatch.
        result.append(string.substring_view(end_of_last_match));
    }

    // 16. Return result.
    return PrimitiveString::create(vm, MUST(result.to_string()));
}

// 22.1.3.21 String.prototype.search ( regexp ), https://tc39.es/ecma262/#sec-string.prototype.search
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::search)
{
    auto regexp = vm.argument(0);

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto this_object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. If regexp is neither undefined nor null, then
    if (!regexp.is_nullish()) {
        // a. Let searcher be ? GetMethod(regexp, @@search).
        auto searcher = TRY(regexp.get_method(vm, vm.well_known_symbol_search()));

        // b. If searcher is not undefined, then
        if (searcher) {
            // i. Return ? Call(searcher, regexp, « O »).
            return TRY(call(vm, *searcher, regexp, this_object));
        }
    }

    // 3. Let string be ? ToString(O).
    auto string = TRY(this_object.to_utf16_string(vm));

    // 4. Let rx be ? RegExpCreate(regexp, undefined).
    auto rx = TRY(regexp_create(vm, regexp, js_undefined()));

    // 5. Return ? Invoke(rx, @@search, « string »).
    return TRY(Value(rx).invoke(vm, vm.well_known_symbol_search(), PrimitiveString::create(vm, move(string))));
}

// 22.1.3.22 String.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-string.prototype.slice
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::slice)
{
    auto start = vm.argument(0);
    auto end = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let len be the length of S.
    auto string_length = static_cast<double>(string.length_in_code_units());

    // 4. Let intStart be ? ToIntegerOrInfinity(start).
    auto int_start = TRY(start.to_integer_or_infinity(vm));

    // 5. If intStart = -∞, let from be 0.
    if (Value(int_start).is_negative_infinity())
        int_start = 0;
    // 6. Else if intStart < 0, let from be max(len + intStart, 0).
    else if (int_start < 0)
        int_start = max(string_length + int_start, 0);
    // 7. Else, let from be min(intStart, len).
    else
        int_start = min(int_start, string_length);

    // 8. If end is undefined, let intEnd be len; else let intEnd be ? ToIntegerOrInfinity(end).
    auto int_end = string_length;
    if (!end.is_undefined()) {
        int_end = TRY(end.to_integer_or_infinity(vm));
        // 9. If intEnd = -∞, let to be 0.
        if (Value(int_end).is_negative_infinity())
            int_end = 0;
        // 10. Else if intEnd < 0, let to be max(len + intEnd, 0).
        else if (int_end < 0)
            int_end = max(string_length + int_end, 0);
        // 11. Else, let to be min(intEnd, len).
        else
            int_end = min(int_end, string_length);
    }

    // 12. If from ≥ to, return the empty String.
    if (int_start >= int_end)
        return PrimitiveString::create(vm, String {});

    // 13. Return the substring of S from from to to.
    return PrimitiveString::create(vm, Utf16String::create(string.substring_view(int_start, int_end - int_start)));
}

// 22.1.3.23 String.prototype.split ( separator, limit ), https://tc39.es/ecma262/#sec-string.prototype.split
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::split)
{
    auto& realm = *vm.current_realm();
    auto separator_argument = vm.argument(0);
    auto limit_argument = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. If separator is neither undefined nor null, then
    if (!separator_argument.is_nullish()) {
        // a. Let splitter be ? GetMethod(separator, @@split).
        auto splitter = TRY(separator_argument.get_method(vm, vm.well_known_symbol_split()));
        // b. If splitter is not undefined, then
        if (splitter) {
            // i. Return ? Call(splitter, separator, « O, limit »).
            return TRY(call(vm, *splitter, separator_argument, object, limit_argument));
        }
    }

    // 3. Let S be ? ToString(O).
    auto string = TRY(object.to_utf16_string(vm));

    // 11. Let substrings be a new empty List.
    auto array = MUST(Array::create(realm, 0));
    size_t array_length = 0;

    // 4. If limit is undefined, let lim be 232 - 1; else let lim be ℝ(? ToUint32(limit)).
    auto limit = NumericLimits<u32>::max();
    if (!limit_argument.is_undefined())
        limit = TRY(limit_argument.to_u32(vm));

    // 5. Let R be ? ToString(separator).
    auto separator = TRY(separator_argument.to_utf16_string(vm));

    // 6. If lim = 0, then
    if (limit == 0) {
        // a. Return CreateArrayFromList(« »).
        return array;
    }

    auto string_length = string.length_in_code_units();

    // 7. If separator is undefined, then
    if (separator_argument.is_undefined()) {
        // a. Return CreateArrayFromList(« S »).
        MUST(array->create_data_property_or_throw(0, PrimitiveString::create(vm, move(string))));
        return array;
    }

    // 8. Let separatorLength be the length of R.
    auto separator_length = separator.length_in_code_units();

    // 10. If S is the empty String, return CreateArrayFromList(« S »).
    if (string_length == 0) {
        if (separator_length > 0)
            MUST(array->create_data_property_or_throw(0, PrimitiveString::create(vm, move(string))));
        return array;
    }

    // 12. Let i be 0.
    size_t start = 0;

    // 13. Let j be StringIndexOf(S, R, 0).
    auto position = start;

    // 14. Repeat, while j ≠ -1,
    while (position != string_length) {
        // a. Let T be the substring of S from i to j.
        auto match = split_match(string.view(), position, separator.view());
        if (!match.has_value() || match.value() == start) {
            ++position;
            continue;
        }
        auto segment = string.substring_view(start, position - start);

        // b. Append T to substrings.
        MUST(array->create_data_property_or_throw(array_length, PrimitiveString::create(vm, Utf16String::create(segment))));
        ++array_length;

        // c. If the number of elements in substrings is lim, return CreateArrayFromList(substrings).
        if (array_length == limit)
            return array;

        // d. Set i to j + separatorLength.
        start = match.value();

        // e. Set j to StringIndexOf(S, R, i).
        position = start;
    }

    // 15. Let T be the substring of S from i.
    auto rest = string.substring_view(start);

    // 16. Append T to substrings.
    MUST(array->create_data_property_or_throw(array_length, PrimitiveString::create(vm, Utf16String::create(rest))));

    // 17. Return CreateArrayFromList(substrings).
    return array;
}

// 22.1.3.24 String.prototype.startsWith ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.startswith
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::starts_with)
{
    auto search_string_value = vm.argument(0);
    auto position = vm.argument(1);

    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let isRegExp be ? IsRegExp(searchString).
    bool is_regexp = TRY(search_string_value.is_regexp(vm));

    // 4. If isRegExp is true, throw a TypeError exception.
    if (is_regexp)
        return vm.throw_completion<TypeError>(ErrorType::IsNotA, "searchString", "string, but a regular expression");

    // 5. Let searchStr be ? ToString(searchString).
    auto search_string = TRY(search_string_value.to_utf16_string(vm));

    // 6. Let len be the length of S.
    auto string_length = string.length_in_code_units();

    size_t start = 0;

    // 7. If position is undefined, let pos be 0; else let pos be ? ToIntegerOrInfinity(position).
    if (!position.is_undefined()) {
        auto pos = TRY(position.to_integer_or_infinity(vm));

        // 8. Let start be the result of clamping pos between 0 and len.
        start = clamp(pos, static_cast<double>(0), static_cast<double>(string_length));
    }

    // 9. Let searchLength be the length of searchStr.
    auto search_length = search_string.length_in_code_units();

    // 10. If searchLength = 0, return true.
    if (search_length == 0)
        return Value(true);

    // 11. Let end be start + searchLength.
    size_t end = start + search_length;

    // 12. If end > len, return false.
    if (end > string_length)
        return Value(false);

    // 13. Let substring be the substring of S from start to end.
    auto substring_view = string.substring_view(start, end - start);

    // 14. If substring is searchStr, return true.
    // 15. Return false.
    return Value(substring_view == search_string.view());
}

// 22.1.3.25 String.prototype.substring ( start, end ), https://tc39.es/ecma262/#sec-string.prototype.substring
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::substring)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let len be the length of S.
    auto string_length = static_cast<double>(string.length_in_code_units());

    // 4. Let intStart be ? ToIntegerOrInfinity(start).
    auto start = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 5. If end is undefined, let intEnd be len; else let intEnd be ? ToIntegerOrInfinity(end).
    auto end = string_length;
    if (!vm.argument(1).is_undefined())
        end = TRY(vm.argument(1).to_integer_or_infinity(vm));

    // 6. Let finalStart be the result of clamping intStart between 0 and len.
    size_t final_start = clamp(start, static_cast<double>(0), string_length);

    // 7. Let finalEnd be the result of clamping intEnd between 0 and len.
    size_t final_end = clamp(end, static_cast<double>(0), string_length);

    // 8. Let from be min(finalStart, finalEnd).
    size_t from = min(final_start, final_end);

    // 9. Let to be max(finalStart, finalEnd).
    size_t to = max(final_start, final_end);

    // 10. Return the substring of S from from to to.
    return PrimitiveString::create(vm, Utf16String::create(string.substring_view(from, to - from)));
}

enum class TargetCase {
    Lower,
    Upper,
};

// 20.1.2.1 TransformCase ( S, locales, targetCase ), https://tc39.es/ecma402/#sec-transform-case
static ThrowCompletionOr<String> transform_case(VM& vm, String const& string, Value locales, TargetCase target_case)
{
    // 1. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(Intl::canonicalize_locale_list(vm, locales));

    String requested_locale;

    // 2. If requestedLocales is not an empty List, then
    if (!requested_locales.is_empty()) {
        // a. Let requestedLocale be requestedLocales[0].
        requested_locale = requested_locales[0];
    }
    // 3. Else,
    else {
        // a. Let requestedLocale be ! DefaultLocale().
        requested_locale = String::from_utf8_without_validation(Unicode::default_locale().bytes());
    }

    // 4. Let availableLocales be an Available Locales List which includes the language tags for which the Unicode Character Database contains language-sensitive case mappings. If the implementation supports additional locale-sensitive case mappings, availableLocales should also include their corresponding language tags.
    // 5. Let match be LookupMatchingLocaleByPrefix(availableLocales, « requestedLocale »).
    auto match = Intl::lookup_matching_locale_by_prefix({ { requested_locale } });

    // 6. If match is not undefined, let locale be match.[[locale]]; else let locale be "und".
    StringView locale = match.has_value() ? match->locale : "und"sv;

    // 7. Let codePoints be StringToCodePoints(S).

    String new_code_points;

    switch (target_case) {
    // 8. If targetCase is lower, then
    case TargetCase::Lower:
        // a. Let newCodePoints be a List whose elements are the result of a lowercase transformation of codePoints according to an implementation-derived algorithm using locale or the Unicode Default Case Conversion algorithm.
        new_code_points = MUST(string.to_lowercase(locale));
        break;
    // 9. Else,
    case TargetCase::Upper:
        // a. Assert: targetCase is upper.
        // b. Let newCodePoints be a List whose elements are the result of an uppercase transformation of codePoints according to an implementation-derived algorithm using locale or the Unicode Default Case Conversion algorithm.
        new_code_points = MUST(string.to_uppercase(locale));
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    // 10. Return CodePointsToString(newCodePoints).
    return new_code_points;
}

// 22.1.3.26 String.prototype.toLocaleLowerCase ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-string.prototype.tolocalelowercase
// 20.1.2 String.prototype.toLocaleLowerCase ( [ locales ] ), https://tc39.es/ecma402/#sup-string.prototype.tolocalelowercase
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::to_locale_lowercase)
{
    auto locales = vm.argument(0);

    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf8_string_from(vm));

    // 3. Return ? TransformCase(S, locales, lower).
    return PrimitiveString::create(vm, TRY(transform_case(vm, string, locales, TargetCase::Lower)));
}

// 22.1.3.27 String.prototype.toLocaleUpperCase ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-string.prototype.tolocaleuppercase
// 20.1.3 String.prototype.toLocaleUpperCase ( [ locales ] ), https://tc39.es/ecma402/#sup-string.prototype.tolocaleuppercase
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::to_locale_uppercase)
{
    auto locales = vm.argument(0);

    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf8_string_from(vm));

    // 3. Return ? TransformCase(S, locales, upper).
    return PrimitiveString::create(vm, TRY(transform_case(vm, string, locales, TargetCase::Upper)));
}

// 22.1.3.28 String.prototype.toLowerCase ( ), https://tc39.es/ecma262/#sec-string.prototype.tolowercase
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::to_lowercase)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    // 3. Let sText be StringToCodePoints(S).
    auto string = TRY(utf8_string_from(vm));

    // 4. Let lowerText be the result of toLowercase(sText), according to the Unicode Default Case Conversion algorithm.
    auto lowercase = MUST(string.to_lowercase());

    // 5. Let L be CodePointsToString(lowerText).
    // 6. Return L.
    return PrimitiveString::create(vm, move(lowercase));
}

// 22.1.3.29 String.prototype.toString ( ), https://tc39.es/ecma262/#sec-string.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::to_string)
{
    // 1. Return ? thisStringValue(this value).
    return TRY(this_string_value(vm, vm.this_value()));
}

// 22.1.3.30 String.prototype.toUpperCase ( ), https://tc39.es/ecma262/#sec-string.prototype.touppercase
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::to_uppercase)
{
    // This method interprets a String value as a sequence of UTF-16 encoded code points, as described in 6.1.4.
    // It behaves in exactly the same way as String.prototype.toLowerCase, except that the String is mapped using the toUppercase algorithm of the Unicode Default Case Conversion.
    auto string = TRY(utf8_string_from(vm));
    auto uppercase = MUST(string.to_uppercase());
    return PrimitiveString::create(vm, move(uppercase));
}

// 22.1.3.31 String.prototype.toWellFormed ( ), https://tc39.es/ecma262/#sec-string.prototype.towellformed
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::to_well_formed)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // NOTE: Rest of steps in to_well_formed below
    return PrimitiveString::create(vm, to_well_formed_string(string));
}

// https://tc39.es/ecma262/#sec-string.prototype.towellformed
String to_well_formed_string(Utf16String const& string)
{
    // 3. Let strLen be the length of S.
    auto length = string.length_in_code_units();

    // 4. Let k be 0.
    size_t k = 0;

    // 5. Let result be the empty String.
    StringBuilder result;

    // 6. Repeat, while k < strLen,
    while (k < length) {
        // a. Let cp be CodePointAt(S, k).
        auto code_point = JS::code_point_at(string.view(), k);

        // b. If cp.[[IsUnpairedSurrogate]] is true, then
        if (code_point.is_unpaired_surrogate) {
            // i. Set result to the string-concatenation of result and 0xFFFD (REPLACEMENT CHARACTER).
            result.append_code_point(0xfffd);
        }
        // c. Else,
        else {
            // i. Set result to the string-concatenation of result and UTF16EncodeCodePoint(cp.[[CodePoint]]).
            result.append_code_point(code_point.code_point);
        }

        // d. Set k to k + cp.[[CodeUnitCount]].
        k += code_point.code_unit_count;
    }

    // 7. Return result.
    return MUST(result.to_string());
}

// 22.1.3.32.1 TrimString ( string, where ), https://tc39.es/ecma262/#sec-trimstring
ThrowCompletionOr<String> trim_string(VM& vm, Value input_value, TrimMode where)
{
    // 1. Let str be ? RequireObjectCoercible(string).
    auto input_string = TRY(require_object_coercible(vm, input_value));

    // 2. Let S be ? ToString(str).
    auto string = TRY(input_string.to_string(vm));

    // 3. If where is start, let T be the String value that is a copy of S with leading white space removed.
    // 4. Else if where is end, let T be the String value that is a copy of S with trailing white space removed.
    // 5. Else,
    // a. Assert: where is start+end.
    // b. Let T be the String value that is a copy of S with both leading and trailing white space removed.
    auto trimmed_string = Utf8View(string).trim(whitespace_characters, where).as_string();

    // 6. Return T.
    return MUST(String::from_utf8(trimmed_string));
}

// 22.1.3.32 String.prototype.trim ( ), https://tc39.es/ecma262/#sec-string.prototype.trim
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::trim)
{
    // 1. Let S be the this value.
    // 2. Return ? TrimString(S, start+end).
    return PrimitiveString::create(vm, TRY(trim_string(vm, vm.this_value(), TrimMode::Both)));
}

// 22.1.3.33 String.prototype.trimEnd ( ), https://tc39.es/ecma262/#sec-string.prototype.trimend
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::trim_end)
{
    // 1. Let S be the this value.
    // 2. Return ? TrimString(S, end).
    return PrimitiveString::create(vm, TRY(trim_string(vm, vm.this_value(), TrimMode::Right)));
}

// 22.1.3.34 String.prototype.trimStart ( ), https://tc39.es/ecma262/#sec-string.prototype.trimstart
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::trim_start)
{
    // 1. Let S be the this value.
    // 2. Return ? TrimString(S, start).
    return PrimitiveString::create(vm, TRY(trim_string(vm, vm.this_value(), TrimMode::Left)));
}

// 22.1.3.35 String.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-string.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::value_of)
{
    return TRY(this_string_value(vm, vm.this_value()));
}

// 22.1.3.36 String.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-string.prototype-@@iterator
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::symbol_iterator)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be ? RequireObjectCoercible(this value).
    auto this_object = TRY(require_object_coercible(vm, vm.this_value()));

    // 2. Let s be ? ToString(O).
    auto string = TRY(this_object.to_string(vm));

    // 3. Let closure be a new Abstract Closure with no parameters that captures s and performs the following steps when called:
    //     ...
    // 4. Return CreateIteratorFromClosure(closure, "%StringIteratorPrototype%", %StringIteratorPrototype%).
    return StringIterator::create(realm, string);
}

// B.2.2.1 String.prototype.substr ( start, length ), https://tc39.es/ecma262/#sec-string.prototype.substr
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::substr)
{
    // 1. Let O be ? RequireObjectCoercible(this value).
    // 2. Let S be ? ToString(O).
    auto string = TRY(utf16_string_from(vm));

    // 3. Let size be the length of S.
    auto size = string.length_in_code_units();

    // 4. Let intStart be ? ToIntegerOrInfinity(start).
    auto int_start = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 5. If intStart is -∞, set intStart to 0.
    if (Value(int_start).is_negative_infinity())
        int_start = 0;
    // 6. Else if intStart < 0, set intStart to max(size + intStart, 0).
    else if (int_start < 0)
        int_start = max(size + int_start, 0);
    // 7. Else, set intStart to min(intStart, size).
    else
        int_start = min(int_start, size);

    // 8. If length is undefined, let intLength be size; otherwise let intLength be ? ToIntegerOrInfinity(length).
    auto length = vm.argument(1);
    auto int_length = length.is_undefined() ? size : TRY(length.to_integer_or_infinity(vm));

    // 9. Set intLength to the result of clamping intLength between 0 and size.
    int_length = clamp(int_length, 0, size);

    // 10. Let intEnd be min(intStart + intLength, size).
    auto int_end = min((i32)(int_start + int_length), size);

    if (int_start >= int_end)
        return PrimitiveString::create(vm, String {});

    // 11. Return the substring of S from intStart to intEnd.
    return PrimitiveString::create(vm, Utf16String::create(string.substring_view(int_start, int_end - int_start)));
}

// B.2.2.2.1 CreateHTML ( string, tag, attribute, value ), https://tc39.es/ecma262/#sec-createhtml
static ThrowCompletionOr<Value> create_html(VM& vm, Value string, StringView tag, StringView attribute, Value value)
{
    // 1. Let str be ? RequireObjectCoercible(string).
    TRY(require_object_coercible(vm, string));

    // 2. Let S be ? ToString(str).
    auto str = TRY(string.to_string(vm));

    // 3. Let p1 be the string-concatenation of "<" and tag.
    StringBuilder builder;
    builder.append('<');
    builder.append(tag);

    // 4. If attribute is not the empty String, then
    if (!attribute.is_empty()) {
        // a. Let V be ? ToString(value).
        auto value_string = TRY(value.to_string(vm));

        // b. Let escapedV be the String value that is the same as V except that each occurrence of the code unit 0x0022 (QUOTATION MARK) in V has been replaced with the six code unit sequence "&quot;".
        auto escaped_value_string = MUST(value_string.replace("\""sv, "&quot;"sv, ReplaceMode::All));

        // c. Set p1 to the string-concatenation of:
        // - p1
        // - the code unit 0x0020 (SPACE)
        builder.append(' ');
        // - attribute
        builder.append(attribute);
        // - the code unit 0x003D (EQUALS SIGN)
        // - the code unit 0x0022 (QUOTATION MARK)
        builder.append("=\""sv);
        // - escapedV
        builder.append(escaped_value_string);
        // - the code unit 0x0022 (QUOTATION MARK)
        builder.append('"');
    }

    // 5. Let p2 be the string-concatenation of p1 and ">".
    builder.append('>');

    // 6. Let p3 be the string-concatenation of p2 and S.
    builder.append(str);

    // 7. Let p4 be the string-concatenation of p3, "</", tag, and ">".
    builder.append("</"sv);
    builder.append(tag);
    builder.append('>');

    // 8. Return p4.
    return PrimitiveString::create(vm, MUST(builder.to_string()));
}

// B.2.2.2 String.prototype.anchor ( name ), https://tc39.es/ecma262/#sec-string.prototype.anchor
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::anchor)
{
    auto name = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "a", "name", name).
    return create_html(vm, vm.this_value(), "a"sv, "name"sv, name);
}

// B.2.2.3 String.prototype.big ( ), https://tc39.es/ecma262/#sec-string.prototype.big
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::big)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "big", "", "").
    return create_html(vm, vm.this_value(), "big"sv, {}, Value());
}

// B.2.2.4 String.prototype.blink ( ), https://tc39.es/ecma262/#sec-string.prototype.blink
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::blink)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "blink", "", "").
    return create_html(vm, vm.this_value(), "blink"sv, {}, Value());
}

// B.2.2.5 String.prototype.bold ( ), https://tc39.es/ecma262/#sec-string.prototype.bold
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::bold)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "b", "", "").
    return create_html(vm, vm.this_value(), "b"sv, {}, Value());
}

// B.2.2.6 String.prototype.fixed ( ), https://tc39.es/ecma262/#sec-string.prototype.fixed
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::fixed)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "tt", "", "").
    return create_html(vm, vm.this_value(), "tt"sv, {}, Value());
}

// B.2.2.7 String.prototype.fontcolor ( color ), https://tc39.es/ecma262/#sec-string.prototype.fontcolor
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::fontcolor)
{
    auto color = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "font", "color", color).
    return create_html(vm, vm.this_value(), "font"sv, "color"sv, color);
}

// B.2.2.8 String.prototype.fontsize ( size ), https://tc39.es/ecma262/#sec-string.prototype.fontsize
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::fontsize)
{
    auto size = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "font", "size", size).
    return create_html(vm, vm.this_value(), "font"sv, "size"sv, size);
}

// B.2.2.9 String.prototype.italics ( ), https://tc39.es/ecma262/#sec-string.prototype.italics
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::italics)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "i", "", "").
    return create_html(vm, vm.this_value(), "i"sv, {}, Value());
}

// B.2.2.10 String.prototype.link ( url ), https://tc39.es/ecma262/#sec-string.prototype.link
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::link)
{
    auto url = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "a", "href", url).
    return create_html(vm, vm.this_value(), "a"sv, "href"sv, url);
}

// B.2.2.11 String.prototype.small ( ), https://tc39.es/ecma262/#sec-string.prototype.small
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::small)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "small", "", "").
    return create_html(vm, vm.this_value(), "small"sv, {}, Value());
}

// B.2.2.12 String.prototype.strike ( ), https://tc39.es/ecma262/#sec-string.prototype.strike
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::strike)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "strike", "", "").
    return create_html(vm, vm.this_value(), "strike"sv, {}, Value());
}

// B.2.2.13 String.prototype.sub ( ), https://tc39.es/ecma262/#sec-string.prototype.sub
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::sub)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "sub", "", "").
    return create_html(vm, vm.this_value(), "sub"sv, {}, Value());
}

// B.2.2.14 String.prototype.sup ( ), https://tc39.es/ecma262/#sec-string.prototype.sup
JS_DEFINE_NATIVE_FUNCTION(StringPrototype::sup)
{
    // 1. Let S be the this value.
    // 2. Return ? CreateHTML(S, "sup", "", "").
    return create_html(vm, vm.this_value(), "sup"sv, {}, Value());
}

}
