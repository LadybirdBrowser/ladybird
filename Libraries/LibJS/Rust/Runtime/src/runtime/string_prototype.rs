/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::{Utf16FlyString, Utf16String};
use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    call, call_function_object, checked_js_string_length_product, checked_js_string_length_sum, get_substitution,
    require_object_coercible,
};
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::define_object_class;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::string_iterator::StringIterator;
use crate::runtime::string_object::{STRING_OBJECT_METHODS, StringObject};
use crate::runtime::value_conversions::MAX_ARRAY_LIKE_INDEX;
use crate::unicode::{NormalizationForm, normalize};
use crate::utf16::{
    TrimMode, Utf16StringBuilder, Utf16View, decode_utf16_surrogate_pair, is_unicode_surrogate, is_utf16_low_surrogate,
    to_well_formed,
};

/// The CodePoint record CodePointAt returns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CodePoint {
    pub is_unpaired_surrogate: bool,
    pub code_point: u32,
    pub code_unit_count: usize,
}

pub const WHITESPACE_CHARACTER_CODE_UNITS: [u16; 25] = [
    0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x0020, 0x00A0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
    0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF,
];

/// %String.prototype%, which is a String exotic object whose [[StringData]] is the empty String.
#[repr(C)]
#[derive(Trace)]
pub struct StringPrototype {
    base: StringObject,
}

define_object_class!(StringPrototype, extends: [StringObject, Object], methods: {
    initialize: StringPrototype::initialize,
    ..STRING_OBJECT_METHODS
});

fn primitive_string_from(vm: &Vm) -> ThrowCompletionOr<Gc<PrimitiveString>> {
    let this_value = require_object_coercible(vm, vm.this_value())?;
    this_value.to_primitive_string(vm)
}

fn string_value(vm: &Vm, string: Utf16String) -> Value {
    Value::from_string(PrimitiveString::create(vm, string))
}

fn empty_string_value(vm: &Vm) -> Value {
    string_value(vm, Utf16String::default())
}

fn substring_value(vm: &Vm, string: Gc<PrimitiveString>, code_unit_offset: usize, code_unit_length: usize) -> Value {
    Value::from_string(PrimitiveString::create_from_substring(
        vm,
        string,
        code_unit_offset,
        code_unit_length,
    ))
}

// 6.1.4.1 StringIndexOf ( string, searchValue, fromIndex ), https://tc39.es/ecma262/#sec-stringindexof
pub fn string_index_of(string: Utf16View<'_>, search_value: Utf16View<'_>, from_index: usize) -> Option<usize> {
    // 1. Let len be the length of string.
    let string_length = string.length_in_code_units();

    // 3. Let searchLen be the length of searchValue.
    let search_length = search_value.length_in_code_units();

    // 2. If searchValue is the empty String and fromIndex ≤ len, return fromIndex.
    if search_length == 0 && from_index <= string_length {
        return Some(from_index);
    }

    // OPTIMIZATION: If the needle is longer than the haystack, don't bother searching :^)
    if search_length > string_length {
        return None;
    }

    // 4. For each integer i such that fromIndex ≤ i ≤ len - searchLen, in ascending order, do
    //    a. Let candidate be the substring of string from i to i + searchLen.
    //    b. If candidate is searchValue, return i.
    // 5. Return -1.
    string.find_code_unit_offset(search_value, from_index)
}

// 6.1.4.2 StringLastIndexOf ( string, searchValue, fromIndex ),
pub fn string_last_index_of(string: Utf16View<'_>, search_value: Utf16View<'_>, from_index: usize) -> Option<usize> {
    // 1. Let len be the length of string.
    let string_length = string.length_in_code_units();

    // 2. Let searchLen be the length of searchValue.
    let search_length = search_value.length_in_code_units();

    // 3. Assert: fromIndex + searchLen ≤ len.
    assert!(from_index + search_length <= string_length);

    // 4. For each integer i such that 0 ≤ i ≤ fromIndex, in descending order, do
    for i in (1..=from_index + 1).rev() {
        // a. Let candidate be the substring of string from i to i + searchLen.
        let candidate = string.substring_view(i - 1, search_length);

        // b. If candidate is searchValue, return i.
        if candidate == search_value {
            return Some(i - 1);
        }
    }

    // 5. Return NOT-FOUND.
    None
}

// 7.2.9 Static Semantics: IsStringWellFormedUnicode ( string )
fn is_string_well_formed_unicode(string: Utf16View<'_>) -> bool {
    // OPTIMIZATION: simdutf can do this much faster.
    string.validate()
}

// 11.1.4 CodePointAt ( string, position ), https://tc39.es/ecma262/#sec-codepointat
pub fn code_point_at(string: Utf16View<'_>, position: usize) -> CodePoint {
    // 1. Let size be the length of string.
    // 2. Assert: position ≥ 0 and position < size.
    assert!(position < string.length_in_code_units());

    // 3. Let first be the code unit at index position within string.
    let first = string.code_unit_at(position);

    // 4. Let cp be the code point whose numeric value is that of first.
    let code_point = u32::from(first);

    // 5. If first is not a leading surrogate or trailing surrogate, then
    if !is_unicode_surrogate(first) {
        // a. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 1, [[IsUnpairedSurrogate]]: false }.
        return CodePoint {
            is_unpaired_surrogate: false,
            code_point,
            code_unit_count: 1,
        };
    }

    // 6. If first is a trailing surrogate or position + 1 = size, then
    if is_utf16_low_surrogate(first) || position + 1 == string.length_in_code_units() {
        // a. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 1, [[IsUnpairedSurrogate]]: true }.
        return CodePoint {
            is_unpaired_surrogate: true,
            code_point,
            code_unit_count: 1,
        };
    }

    // 7. Let second be the code unit at index position + 1 within string.
    let second = string.code_unit_at(position + 1);

    // 8. If second is not a trailing surrogate, then
    if !is_utf16_low_surrogate(second) {
        // a. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 1, [[IsUnpairedSurrogate]]: true }.
        return CodePoint {
            is_unpaired_surrogate: true,
            code_point,
            code_unit_count: 1,
        };
    }

    // 9. Set cp to UTF16SurrogatePairToCodePoint(first, second).
    // 10. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 2, [[IsUnpairedSurrogate]]: false }.
    CodePoint {
        is_unpaired_surrogate: false,
        code_point: decode_utf16_surrogate_pair(first, second),
        code_unit_count: 2,
    }
}

/// Mirrors the C++ clamp(value, min, max) of doubles, followed by the conversion of the clamped value to size_t.
fn clamp_to_index(value: f64, max: usize) -> usize {
    value.clamp(0.0, max as f64) as usize
}

impl StringPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<StringPrototype> {
        realm.create_object(
            vm,
            StringPrototype {
                base: StringObject::new(vm, Self::CLASS, vm.empty_string(), realm.object_prototype()),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        StringObject::initialize(object, vm, realm);
        let names = &vm.names;
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |property_key: &PropertyKey, function, length, builtin| {
            object.define_native_function(vm, realm, property_key, function, length, attributes, builtin);
        };

        // 22.1.3 Properties of the String Prototype Object, https://tc39.es/ecma262/#sec-properties-of-the-string-prototype-object
        define(&names.at, raw_native!(StringPrototype::at), 1, None);
        define(
            &names.charAt,
            raw_native!(StringPrototype::char_at),
            1,
            Some(Builtin::StringPrototypeCharAt),
        );
        define(
            &names.charCodeAt,
            raw_native!(StringPrototype::char_code_at),
            1,
            Some(Builtin::StringPrototypeCharCodeAt),
        );
        define(&names.codePointAt, raw_native!(StringPrototype::code_point_at), 1, None);
        define(&names.concat, raw_native!(StringPrototype::concat), 1, None);
        define(&names.endsWith, raw_native!(StringPrototype::ends_with), 1, None);
        define(&names.includes, raw_native!(StringPrototype::includes), 1, None);
        define(&names.indexOf, raw_native!(StringPrototype::index_of), 1, None);
        define(
            &names.isWellFormed,
            raw_native!(StringPrototype::is_well_formed),
            0,
            None,
        );
        define(&names.lastIndexOf, raw_native!(StringPrototype::last_index_of), 1, None);
        define(
            &names.localeCompare,
            raw_native!(StringPrototype::locale_compare),
            1,
            None,
        );
        define(&names.match_, raw_native!(StringPrototype::match_), 1, None);
        define(&names.matchAll, raw_native!(StringPrototype::match_all), 1, None);
        define(&names.normalize, raw_native!(StringPrototype::normalize), 0, None);
        define(&names.padEnd, raw_native!(StringPrototype::pad_end), 1, None);
        define(&names.padStart, raw_native!(StringPrototype::pad_start), 1, None);
        define(&names.repeat, raw_native!(StringPrototype::repeat), 1, None);
        define(&names.replace, raw_native!(StringPrototype::replace), 2, None);
        define(&names.replaceAll, raw_native!(StringPrototype::replace_all), 2, None);
        define(&names.search, raw_native!(StringPrototype::search), 1, None);
        define(&names.slice, raw_native!(StringPrototype::slice), 2, None);
        define(&names.split, raw_native!(StringPrototype::split), 2, None);
        define(&names.startsWith, raw_native!(StringPrototype::starts_with), 1, None);
        define(&names.substring, raw_native!(StringPrototype::substring), 2, None);
        define(
            &names.toLocaleLowerCase,
            raw_native!(StringPrototype::to_locale_lowercase),
            0,
            None,
        );
        define(
            &names.toLocaleUpperCase,
            raw_native!(StringPrototype::to_locale_uppercase),
            0,
            None,
        );
        define(&names.toLowerCase, raw_native!(StringPrototype::to_lowercase), 0, None);
        define(&names.toString, raw_native!(StringPrototype::to_string), 0, None);
        define(&names.toUpperCase, raw_native!(StringPrototype::to_uppercase), 0, None);
        define(
            &names.toWellFormed,
            raw_native!(StringPrototype::to_well_formed),
            0,
            None,
        );
        define(&names.trim, raw_native!(StringPrototype::trim), 0, None);
        define(&names.trimEnd, raw_native!(StringPrototype::trim_end), 0, None);
        define(&names.trimStart, raw_native!(StringPrototype::trim_start), 0, None);
        define(&names.valueOf, raw_native!(StringPrototype::value_of), 0, None);
        define(
            &PropertyKey::from(vm.well_known_symbols().iterator),
            raw_native!(StringPrototype::symbol_iterator),
            0,
            None,
        );

        // B.2.2 Additional Properties of the String.prototype Object, https://tc39.es/ecma262/#sec-additional-properties-of-the-string.prototype-object
        define(&names.substr, raw_native!(StringPrototype::substr), 2, None);
        define(&names.anchor, raw_native!(StringPrototype::anchor), 1, None);
        define(&names.big, raw_native!(StringPrototype::big), 0, None);
        define(&names.blink, raw_native!(StringPrototype::blink), 0, None);
        define(&names.bold, raw_native!(StringPrototype::bold), 0, None);
        define(&names.fixed, raw_native!(StringPrototype::fixed), 0, None);
        define(&names.fontcolor, raw_native!(StringPrototype::fontcolor), 1, None);
        define(&names.fontsize, raw_native!(StringPrototype::fontsize), 1, None);
        define(&names.italics, raw_native!(StringPrototype::italics), 0, None);
        define(&names.link, raw_native!(StringPrototype::link), 1, None);
        define(&names.small, raw_native!(StringPrototype::small), 0, None);
        define(&names.strike, raw_native!(StringPrototype::strike), 0, None);
        define(&names.sub, raw_native!(StringPrototype::sub), 0, None);
        define(&names.sup, raw_native!(StringPrototype::sup), 0, None);
        object.define_direct_property(
            vm,
            &names.trimLeft,
            object.get_without_side_effects(vm, &names.trimStart),
            attributes,
        );
        object.define_direct_property(
            vm,
            &names.trimRight,
            object.get_without_side_effects(vm, &names.trimEnd),
            attributes,
        );
    }

    // 22.1.3.1 String.prototype.at ( index ), https://tc39.es/ecma262/#sec-string.prototype.at
    fn at(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? ToObject(this value).
        let string = primitive_string_from(vm)?;
        // 2. Let len be ? LengthOfArrayLike(O).
        let length = string.length_in_utf16_code_units();

        // 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
        let relative_index = vm.argument(0).to_integer_or_infinity(vm)?;
        if relative_index.is_infinite() {
            return Ok(Value::UNDEFINED);
        }

        // 4. If relativeIndex ≥ 0, then
        let index = if relative_index >= 0.0 {
            // a. Let k be relativeIndex.
            relative_index
        }
        // 5. Else,
        else {
            // a. Let k be len + relativeIndex.
            length as f64 + relative_index
        };

        // 6. If k < 0 or k ≥ len, return undefined.
        if index < 0.0 || index >= length as f64 {
            return Ok(Value::UNDEFINED);
        }

        // 7. Return ? Get(O, ! ToString(𝔽(k))).
        Ok(substring_value(vm, string, index as usize, 1))
    }

    // 22.1.3.2 String.prototype.charAt ( pos ), https://tc39.es/ecma262/#sec-string.prototype.charat
    fn char_at(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let position be ? ToIntegerOrInfinity(pos).
        let position = vm.argument(0).to_integer_or_infinity(vm)?;

        // 4. Let size be the length of S.
        // 5. If position < 0 or position ≥ size, return the empty String.
        if position < 0.0 || position >= string.length_in_utf16_code_units() as f64 {
            return Ok(empty_string_value(vm));
        }

        // 6. Return the substring of S from position to position + 1.
        Ok(substring_value(vm, string, position as usize, 1))
    }

    // 22.1.3.3 String.prototype.charCodeAt ( pos ), https://tc39.es/ecma262/#sec-string.prototype.charcodeat
    fn char_code_at(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let position be ? ToIntegerOrInfinity(pos).
        let position = vm.argument(0).to_integer_or_infinity(vm)?;

        // 4. Let size be the length of S.
        // 5. If position < 0 or position ≥ size, return NaN.
        if position < 0.0 || position >= string.length_in_utf16_code_units() as f64 {
            return Ok(Value::from_f64(f64::NAN));
        }

        // 6. Return the Number value for the numeric value of the code unit at index position within the String S.
        Ok(Value::from_i32(i32::from(string.code_unit_at(position as usize))))
    }

    // 22.1.3.4 String.prototype.codePointAt ( pos ), https://tc39.es/ecma262/#sec-string.prototype.codepointat
    fn code_point_at(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let position be ? ToIntegerOrInfinity(pos).
        let position = vm.argument(0).to_integer_or_infinity(vm)?;

        // 4. Let size be the length of S.
        // 5. If position < 0 or position ≥ size, return undefined.
        if position < 0.0 || position >= string.length_in_utf16_code_units() as f64 {
            return Ok(Value::UNDEFINED);
        }

        // 6. Let cp be CodePointAt(S, position).
        let code_point = code_point_at(string.utf16_string_view(), position as usize);

        // 7. Return 𝔽(cp.[[CodePoint]]).
        Ok(Value::from_f64(f64::from(code_point.code_point)))
    }

    // 22.1.3.5 String.prototype.concat ( ...args ), https://tc39.es/ecma262/#sec-string.prototype.concat
    fn concat(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        let object = require_object_coercible(vm, vm.this_value())?;

        // 2. Let S be ? ToString(O).
        let string = object.to_primitive_string(vm)?;

        // 3. Let R be S.
        let mut result = string;

        // 4. For each element next of args, do
        for i in 0..vm.argument_count() {
            // a. Let nextString be ? ToString(next).
            let next_string = vm.argument(i).to_primitive_string(vm)?;

            // b. Set R to the string-concatenation of R and nextString.
            result = PrimitiveString::create_from_concatenation(vm, result, next_string)?;
        }

        // 5. Return R.
        Ok(Value::from_string(result))
    }

    // 22.1.3.7 String.prototype.endsWith ( searchString [ , endPosition ] ), https://tc39.es/ecma262/#sec-string.prototype.endswith
    fn ends_with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_string_value = vm.argument(0);
        let end_position = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // Let isRegExp be ? IsRegExp(searchString).
        let is_regexp = search_string_value.is_regexp(vm)?;

        // 4. If isRegExp is true, throw a TypeError exception.
        if is_regexp {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IsNotA,
                &[&"searchString", &"string, but a regular expression"],
            );
        }

        // 5. Let searchStr be ? ToString(searchString).
        let search_string = search_string_value.to_utf16_string(vm)?;

        // 6. Let len be the length of S.
        let string_length = string.length_in_utf16_code_units();

        // 7. If endPosition is undefined, let pos be len; else let pos be ? ToIntegerOrInfinity(endPosition).
        let mut end = string_length;
        if !end_position.is_undefined() {
            let position = end_position.to_integer_or_infinity(vm)?;

            // 8. Let end be the result of clamping pos between 0 and len.
            end = clamp_to_index(position, string_length);
        }

        // 9. Let searchLength be the length of searchStr.
        let search_view = Utf16View::of_string(&search_string);
        let search_length = search_view.length_in_code_units();

        // 10. If searchLength = 0, return true.
        if search_length == 0 {
            return Ok(Value::TRUE);
        }

        // 12. If start < 0, return false.
        if search_length > end {
            return Ok(Value::FALSE);
        }

        // 11. Let start be end - searchLength.
        let start = end - search_length;

        // 13. Let substring be the substring of S from start to end.
        let substring_view = string.utf16_string_view().substring_view(start, end - start);

        // 14. If substring is searchStr, return true.
        // 15. Return false.
        Ok(Value::from_bool(substring_view == search_view))
    }

    // 22.1.3.8 String.prototype.includes ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.includes
    fn includes(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_string_value = vm.argument(0);
        let position = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let isRegExp be ? IsRegExp(searchString).
        let is_regexp = search_string_value.is_regexp(vm)?;

        // 4. If isRegExp is true, throw a TypeError exception.
        if is_regexp {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IsNotA,
                &[&"searchString", &"string, but a regular expression"],
            );
        }

        // 5. Let searchStr be ? ToString(searchString).
        let search_string = search_string_value.to_utf16_string(vm)?;

        let mut start = 0;
        if !position.is_undefined() {
            // 6. Let pos be ? ToIntegerOrInfinity(position).
            // 7. Assert: If position is undefined, then pos is 0.
            let pos = position.to_integer_or_infinity(vm)?;

            // 8. Let len be the length of S.
            // 9. Let start be the result of clamping pos between 0 and len.
            start = clamp_to_index(pos, string.length_in_utf16_code_units());
        }

        // 10. Let index be StringIndexOf(S, searchStr, start).
        let index = string_index_of(string.utf16_string_view(), Utf16View::of_string(&search_string), start);

        // 11. If index ≠ -1, return true.
        // 12. Return false.
        Ok(Value::from_bool(index.is_some()))
    }

    // 22.1.3.9 String.prototype.indexOf ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.indexof
    fn index_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let searchStr be ? ToString(searchString).
        let search_string = vm.argument(0).to_utf16_string(vm)?;

        let mut start = 0;
        if vm.argument_count() > 1 {
            // 4. Let pos be ? ToIntegerOrInfinity(position).
            // 5. Assert: If position is undefined, then pos is 0.
            let position = vm.argument(1).to_integer_or_infinity(vm)?;

            // 6. Let len be the length of S.
            // 7. Let start be the result of clamping pos between 0 and len.
            start = clamp_to_index(position, string.length_in_utf16_code_units());
        }

        // 8. Return 𝔽(StringIndexOf(S, searchStr, start)).
        let index = string_index_of(string.utf16_string_view(), Utf16View::of_string(&search_string), start);
        Ok(index.map_or(Value::from_i32(-1), |index| Value::from_f64(index as f64)))
    }

    // 22.1.3.10 String.prototype.isWellFormed ( ), https://tc39.es/ecma262/#sec-string.prototype.iswellformed
    fn is_well_formed(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Return IsStringWellFormedUnicode(S).
        Ok(Value::from_bool(is_string_well_formed_unicode(
            string.utf16_string_view(),
        )))
    }

    // 22.1.3.11 String.prototype.lastIndexOf ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.lastindexof
    fn last_index_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireObjectCoercible(O).
        // 3. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 4. Let searchStr be ? ToString(searchString).
        let search_string = vm.argument(0).to_utf16_string(vm)?;

        // 5. Let numPos be ? ToNumber(position).
        // 6. Assert: If position is undefined, then numPos is NaN.
        let position = vm.argument(1).to_number(vm)?;

        // 7. If numPos is NaN, let pos be +∞; otherwise let pos be ! ToIntegerOrInfinity(numPos).
        let pos = if position.is_nan() {
            f64::INFINITY
        } else {
            position.to_integer_or_infinity(vm).must()
        };

        // 8. Let len be the length of S.
        let string_length = string.length_in_utf16_code_units();

        // 9. Let searchLen be the length of searchStr.
        let search_view = Utf16View::of_string(&search_string);
        let search_length = search_view.length_in_code_units();

        // 10. If len < searchLen, return -1𝔽.
        if string_length < search_length {
            return Ok(Value::from_i32(-1));
        }

        // 11. Let start be the result of clamping pos between 0 and len - searchLen.
        let start = clamp_to_index(pos, string_length - search_length);

        // 12. Let result be StringLastIndexOf(S, searchStr, start).
        let result = string_last_index_of(string.utf16_string_view(), search_view, start);

        // 13. If result is NOT-FOUND, return -1𝔽.
        // 14. Return 𝔽(result).
        Ok(result.map_or(Value::from_i32(-1), |result| Value::from_f64(result as f64)))
    }

    // 22.1.3.12 String.prototype.localeCompare ( that [ , reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-string.prototype.localecompare
    // 20.1.1 String.prototype.localeCompare ( that [ , locales [ , options ] ] ), https://tc39.es/ecma402/#sup-String.prototype.localeCompare
    fn locale_compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");

        // 1. Let O be ? RequireObjectCoercible(this value).
        let object = require_object_coercible(vm, vm.this_value())?;

        // 2. Let S be ? ToString(O).
        let string = object.to_utf16_string(vm)?;

        // 3. Let thatValue be ? ToString(that).
        let that_value = vm.argument(0).to_utf16_string(vm)?;

        // 4. Let collator be ? Construct(%Collator%, « locales, options »).
        let locales = vm.argument(1);
        let options = vm.argument(2);

        // OPTIMIZATION: If both locales and options are undefined, we can use a cached default-constructed Collator.
        if locales.is_undefined() && options.is_undefined() {
            // OPTIMIZATION: Identical strings are equal with the default options.
            if string == that_value {
                return Ok(Value::from_i32(0));
            }
            // OPTIMIZATION: If both strings are ASCII, use a comparison that doesn't invoke ICU.
            if let (Utf16View::Ascii(string), Utf16View::Ascii(that_value)) =
                (Utf16View::of_string(&string), Utf16View::of_string(&that_value))
                && let Some(result) = try_fast_ascii_string_compare(string, that_value)
            {
                return Ok(Value::from_i32(result));
            }
            realm.intrinsics().default_collator(vm);
        } else {
            realm.intrinsics().intl_collator_constructor(vm);
        }

        // 5. Return CompareStrings(collator, S, thatValue).
        unimplemented_runtime_function(
            "Intl::compare_strings in String.prototype.localeCompare, which needs Intl.Collator",
            0,
        )
    }

    // 22.1.3.13 String.prototype.match ( regexp ), https://tc39.es/ecma262/#sec-string.prototype.match
    fn match_(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        let this_object = require_object_coercible(vm, vm.this_value())?;

        // 2. If regexp is an Object, then
        let regexp = vm.argument(0);
        if regexp.is_object() {
            // a. Let matcher be ? GetMethod(regexp, @@match).
            let matcher = regexp.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().match_),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::StringPrototypeMatchMatcher),
            )?;

            // b. If matcher is not undefined, then
            if let Some(matcher) = matcher {
                // i. Return ? Call(matcher, regexp, « O »).
                return call_function_object(vm, matcher, regexp, &[this_object]);
            }
        }

        // 3. Let S be ? ToString(O).
        let string = this_object.to_primitive_string(vm)?;

        // 4. Let rx be ? RegExpCreate(regexp, undefined).
        // 5. Return ? Invoke(rx, @@match, « S »).
        regexp_create_and_invoke(vm, regexp, Value::UNDEFINED, string, "@@match")
    }

    // 22.1.3.14 String.prototype.matchAll ( regexp ), https://tc39.es/ecma262/#sec-string.prototype.matchall
    fn match_all(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp = vm.argument(0);

        // 1. Let O be ? RequireObjectCoercible(this value).
        let this_object = require_object_coercible(vm, vm.this_value())?;

        // 2. If regexp is an Object, then
        if regexp.is_object() {
            // a. Let isRegExp be ? IsRegExp(regexp).
            let is_regexp = regexp.is_regexp(vm)?;

            // b. If isRegExp is true, then
            if is_regexp {
                // i. Let flags be ? Get(regexp, "flags").
                let flags = regexp.as_object().get(vm, &vm.names.flags)?;

                // ii. Perform ? RequireObjectCoercible(flags).
                let flags_object = require_object_coercible(vm, flags)?;

                // iii. If ? ToString(flags) does not contain "g", throw a TypeError exception.
                let flags_string = flags_object.to_utf16_string(vm)?;
                if !Utf16View::of_string(&flags_string)
                    .code_units()
                    .any(|code_unit| code_unit == u16::from(b'g'))
                {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::StringNonGlobalRegExp, &[]);
                }
            }

            // c. Let matcher be ? GetMethod(regexp, @@matchAll).
            let matcher = regexp.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().match_all),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::StringPrototypeMatchAllMatcher),
            )?;

            // d. If matcher is not undefined, then
            if let Some(matcher) = matcher {
                // i. Return ? Call(matcher, regexp, « O »).
                return call_function_object(vm, matcher, regexp, &[this_object]);
            }
        }

        // 3. Let S be ? ToString(O).
        let string = this_object.to_primitive_string(vm)?;

        // 4. Let rx be ? RegExpCreate(regexp, "g").
        // 5. Return ? Invoke(rx, @@matchAll, « S »).
        let flags = Value::from_string(PrimitiveString::create_from_fly_string(
            vm,
            &Utf16FlyString::from_utf8("g"),
        ));
        regexp_create_and_invoke(vm, regexp, flags, string, "@@matchAll")
    }

    // 22.1.3.15 String.prototype.normalize ( [ form ] ), https://tc39.es/ecma262/#sec-string.prototype.normalize
    fn normalize(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        let form_value = vm.argument(0);

        // 3. If form is undefined, let f be "NFC".
        let form = if form_value.is_undefined() {
            Utf16String::from_utf8("NFC")
        }
        // 4. Else, let f be ? ToString(form).
        else {
            form_value.to_utf16_string(vm)?
        };

        // 5. If f is not one of "NFC", "NFD", "NFKC", or "NFKD", throw a RangeError exception.
        let form_view = Utf16View::of_string(&form);
        if !["NFC", "NFD", "NFKC", "NFKD"].iter().any(|name| form_view == *name) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::InvalidNormalizationForm,
                &[&form_view.to_utf8()],
            );
        }

        // 6. Let ns be the String value that is the result of normalizing S into the normalization form named by f as specified in https://unicode.org/reports/tr15/.
        let form = if form_view == "NFD" {
            NormalizationForm::NFD
        } else if form_view == "NFC" {
            NormalizationForm::NFC
        } else if form_view == "NFKD" {
            NormalizationForm::NFKD
        } else {
            NormalizationForm::NFKC
        };
        let normalized = normalize(string.utf16_string_view(), form);

        // 7. Return ns.
        Ok(Value::from_string(PrimitiveString::create(vm, normalized)))
    }

    // 22.1.3.16 String.prototype.padEnd ( maxLength [ , fillString ] ), https://tc39.es/ecma262/#sec-string.prototype.padend
    fn pad_end(vm: &Vm) -> ThrowCompletionOr<Value> {
        let max_length = vm.argument(0);
        let fill_string = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        let string = primitive_string_from(vm)?;

        // 2. Return ? StringPad(O, maxLength, fillString, end).
        pad_string(vm, string, max_length, fill_string, PadPlacement::End)
    }

    // 22.1.3.17 String.prototype.padStart ( maxLength [ , fillString ] ), https://tc39.es/ecma262/#sec-string.prototype.padstart
    fn pad_start(vm: &Vm) -> ThrowCompletionOr<Value> {
        let max_length = vm.argument(0);
        let fill_string = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        let string = primitive_string_from(vm)?;

        // 2. Return ? StringPad(O, maxLength, fillString, start).
        pad_string(vm, string, max_length, fill_string, PadPlacement::Start)
    }

    // 22.1.3.18 String.prototype.repeat ( count ), https://tc39.es/ecma262/#sec-string.prototype.repeat
    fn repeat(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let n be ? ToIntegerOrInfinity(count).
        let n = vm.argument(0).to_integer_or_infinity(vm)?;

        // 4. If n < 0 or n = +∞, throw a RangeError exception.
        if n < 0.0 {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::StringRepeatCountMustBe,
                &[&"positive"],
            );
        }
        if n == f64::INFINITY {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::StringRepeatCountMustBe, &[&"finite"]);
        }

        // 5. If n = 0, return the empty String.
        if n == 0.0 {
            return Ok(empty_string_value(vm));
        }

        // OPTIMIZATION: If the string is empty, the result will be empty as well.
        if string.is_empty() {
            return Ok(empty_string_value(vm));
        }

        if n > MAX_ARRAY_LIKE_INDEX {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::StringRepeatCountMustNotOverflow, &[]);
        }

        let count = n as usize;
        let string = string.utf16_string();
        let string_view = Utf16View::of_string(&string);

        checked_js_string_length_product(
            vm,
            string_view.length_in_code_units(),
            count,
            ErrorType::StringRepeatCountMustNotOverflow,
        )?;

        // 6. Return the String value that is made from n copies of S appended together.
        let mut builder = Utf16StringBuilder::new();
        builder.append_repeated(string_view, count);
        Ok(string_value(vm, builder.to_utf16_string()))
    }

    // 22.1.3.19 String.prototype.replace ( searchValue, replaceValue ), https://tc39.es/ecma262/#sec-string.prototype.replace
    fn replace(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_value = vm.argument(0);
        let replace_value = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        let this_object = require_object_coercible(vm, vm.this_value())?;

        // 2. If searchValue is an Object, then
        if search_value.is_object() {
            // a. Let replacer be ? GetMethod(searchValue, @@replace).
            let replacer = search_value.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().replace),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::StringPrototypeReplaceReplacer),
            )?;

            // b. If replacer is not undefined, then
            if let Some(replacer) = replacer {
                if replacer.builtin() == Some(Builtin::RegExpPrototypeReplace) && replacer.realm() == vm.current_realm()
                {
                    // OPTIMIZATION: The common case of RegExp.prototype[@@replace]
                    this_object.to_primitive_string(vm)?;
                    unimplemented_runtime_function(
                        "RegExpPrototype::symbol_replace_impl in String.prototype.replace, which needs the RegExp \
                         builtins",
                        0,
                    );
                }
                // i. Return ? Call(replacer, searchValue, « O, replaceValue »).
                return call_function_object(vm, replacer, search_value, &[this_object, replace_value]);
            }
        }

        // 3. Let string be ? ToString(O).
        let string = this_object.to_primitive_string(vm)?;

        // 4. Let searchString be ? ToString(searchValue).
        let search_string = search_value.to_primitive_string(vm)?;

        // 5. Let functionalReplace be IsCallable(replaceValue).
        // 6. If functionalReplace is false, then
        let mut replace_string = None;
        if !replace_value.is_function() {
            // a. Set replaceValue to ? ToString(replaceValue).
            replace_string = Some(replace_value.to_utf16_string(vm)?);
        }

        // 7. Let searchLength be the length of searchString.
        let search_length = search_string.length_in_utf16_code_units();

        // 8. Let position be StringIndexOf(string, searchString, 0).
        let string_data = string.utf16_string();
        let string_view = Utf16View::of_string(&string_data);
        let search_string_data = search_string.utf16_string();
        let search_string_view = Utf16View::of_string(&search_string_data);
        let position = string_index_of(string_view, search_string_view, 0);

        // 9. If position = -1, return string.
        let Some(position) = position else {
            return Ok(Value::from_string(string));
        };

        // 10. Let preceding be the substring of string from 0 to position.
        let preceding = string_view.substring_view(0, position);

        // 11. Let following be the substring of string from position + searchLength.
        let following_start = position + search_length;
        let following =
            string_view.substring_view(following_start, string_view.length_in_code_units() - following_start);

        // 12. If functionalReplace is true, then
        let replacement = if replace_value.is_function() {
            // a. Let replacement be ? ToString(? Call(replaceValue, undefined, « searchString, 𝔽(position), string »)).
            let result = call(
                vm,
                replace_value,
                Value::UNDEFINED,
                &[
                    Value::from_string(search_string),
                    Value::from_f64(position as f64),
                    Value::from_string(string),
                ],
            )?;
            result.to_utf16_string(vm)?
        }
        // 13. Else,
        else {
            // a. Assert: replaceValue is a String.
            let replace_string =
                replace_string.expect("a replacement that is not a function was converted to a string");

            // b. Let captures be a new empty List.
            // c. Let replacement be ! GetSubstitution(searchString, string, position, captures, undefined, replaceValue).
            get_substitution(
                vm,
                search_string_view,
                string_view,
                position,
                &[],
                Value::UNDEFINED,
                Utf16View::of_string(&replace_string),
            )?
        };

        // 14. Return the string-concatenation of preceding, replacement, and following.
        let mut builder = Utf16StringBuilder::new();
        builder.append(preceding);
        builder.append(Utf16View::of_string(&replacement));
        builder.append(following);

        Ok(string_value(vm, builder.to_utf16_string()))
    }

    // 22.1.3.20 String.prototype.replaceAll ( searchValue, replaceValue ), https://tc39.es/ecma262/#sec-string.prototype.replaceall
    fn replace_all(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_value = vm.argument(0);
        let replace_value = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        let this_object = require_object_coercible(vm, vm.this_value())?;

        // 2. If searchValue is an Object, then
        if search_value.is_object() {
            // a. Let isRegExp be ? IsRegExp(searchValue).
            let is_regexp = search_value.is_regexp(vm)?;

            // b. If isRegExp is true, then
            if is_regexp {
                // i. Let flags be ? Get(searchValue, "flags").
                let flags = search_value.as_object().get(vm, &vm.names.flags)?;

                // ii. Perform ? RequireObjectCoercible(flags).
                let flags_object = require_object_coercible(vm, flags)?;

                // iii. If ? ToString(flags) does not contain "g", throw a TypeError exception.
                let flags_string = flags_object.to_utf16_string(vm)?;
                if !Utf16View::of_string(&flags_string)
                    .code_units()
                    .any(|code_unit| code_unit == u16::from(b'g'))
                {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::StringNonGlobalRegExp, &[]);
                }
            }

            // c. Let replacer be ? GetMethod(searchValue, @@replace).
            let replacer = search_value.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().replace),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::StringPrototypeReplaceAllReplacer),
            )?;

            // d. If replacer is not undefined, then
            if let Some(replacer) = replacer {
                if replacer.builtin() == Some(Builtin::RegExpPrototypeReplace) {
                    // OPTIMIZATION: The common case of RegExp.prototype[@@replace]
                    this_object.to_primitive_string(vm)?;
                    unimplemented_runtime_function(
                        "RegExpPrototype::symbol_replace_impl in String.prototype.replaceAll, which needs the RegExp \
                         builtins",
                        0,
                    );
                }
                // i. Return ? Call(replacer, searchValue, « O, replaceValue »).
                return call_function_object(vm, replacer, search_value, &[this_object, replace_value]);
            }
        }

        // 3. Let string be ? ToString(O).
        let string = this_object.to_primitive_string(vm)?;

        // 4. Let searchString be ? ToString(searchValue).
        let search_string = search_value.to_primitive_string(vm)?;

        // 5. Let functionalReplace be IsCallable(replaceValue).
        // 6. If functionalReplace is false, then
        let mut replace_string = None;
        if !replace_value.is_function() {
            // a. Set replaceValue to ? ToString(replaceValue).
            replace_string = Some(replace_value.to_utf16_string(vm)?);
        }

        // 7. Let searchLength be the length of searchString.
        let search_length = search_string.length_in_utf16_code_units();

        // 8. Let advanceBy be max(1, searchLength).
        let advance_by = search_length.max(1);

        // 9. Let matchPositions be a new empty List.
        let mut match_positions = Vec::new();

        // 10. Let position be StringIndexOf(string, searchString, 0).
        let string_data = string.utf16_string();
        let string_view = Utf16View::of_string(&string_data);
        let search_string_data = search_string.utf16_string();
        let search_string_view = Utf16View::of_string(&search_string_data);
        let mut position = string_index_of(string_view, search_string_view, 0);

        // 11. Repeat, while position ≠ -1,
        while let Some(match_position) = position {
            // a. Append position to matchPositions.
            match_positions.push(match_position);

            // b. Set position to StringIndexOf(string, searchString, position + advanceBy).
            position = string_index_of(string_view, search_string_view, match_position + advance_by);
        }

        // 12. Let endOfLastMatch be 0.
        let mut end_of_last_match = 0;

        // 13. Let result be the empty String.
        let mut result = Utf16StringBuilder::new();

        // 14. For each element p of matchPositions, do
        for position in match_positions {
            // a. Let preserved be the substring of string from endOfLastMatch to p.
            let preserved = string_view.substring_view(end_of_last_match, position - end_of_last_match);

            // b. If functionalReplace is true, then
            let replacement = if replace_value.is_function() {
                // i. Let replacement be ? ToString(? Call(replaceValue, undefined, « searchString, 𝔽(p), string »)).
                call(
                    vm,
                    replace_value,
                    Value::UNDEFINED,
                    &[
                        Value::from_string(search_string),
                        Value::from_f64(position as f64),
                        Value::from_string(string),
                    ],
                )?
                .to_utf16_string(vm)?
            }
            // c. Else,
            else {
                // i. Assert: replaceValue is a String.
                let replace_string = replace_string
                    .as_ref()
                    .expect("a replacement that is not a function was converted to a string");
                // ii. Let captures be a new empty List.
                // iii. Let replacement be ! GetSubstitution(searchString, string, p, captures, undefined, replaceValue).
                get_substitution(
                    vm,
                    search_string_view,
                    string_view,
                    position,
                    &[],
                    Value::UNDEFINED,
                    Utf16View::of_string(replace_string),
                )?
            };

            // d. Set result to the string-concatenation of result, preserved, and replacement.
            result.append(preserved);
            result.append(Utf16View::of_string(&replacement));

            // e. Set endOfLastMatch to p + searchLength.
            end_of_last_match = position + search_length;
        }

        let string_length = string_view.length_in_code_units();

        // 15. If endOfLastMatch < the length of string, then
        if end_of_last_match < string_length {
            // a. Set result to the string-concatenation of result and the substring of string from endOfLastMatch.
            result.append(string_view.substring_view(end_of_last_match, string_length - end_of_last_match));
        }

        // 16. Return result.
        Ok(string_value(vm, result.to_utf16_string()))
    }

    // 22.1.3.21 String.prototype.search ( regexp ), https://tc39.es/ecma262/#sec-string.prototype.search
    fn search(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp = vm.argument(0);

        // 1. Let O be ? RequireObjectCoercible(this value).
        let this_object = require_object_coercible(vm, vm.this_value())?;

        // 2. If regexp is an Object, then
        if regexp.is_object() {
            // a. Let searcher be ? GetMethod(regexp, @@search).
            let searcher = regexp.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().search),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::StringPrototypeSearchSearcher),
            )?;

            // b. If searcher is not undefined, then
            if let Some(searcher) = searcher {
                // i. Return ? Call(searcher, regexp, « O »).
                return call_function_object(vm, searcher, regexp, &[this_object]);
            }
        }

        // 3. Let string be ? ToString(O).
        let string = this_object.to_primitive_string(vm)?;

        // 4. Let rx be ? RegExpCreate(regexp, undefined).
        // 5. Return ? Invoke(rx, @@search, « string »).
        regexp_create_and_invoke(vm, regexp, Value::UNDEFINED, string, "@@search")
    }

    // 22.1.3.22 String.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-string.prototype.slice
    fn slice(vm: &Vm) -> ThrowCompletionOr<Value> {
        let start = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let len be the length of S.
        let string_length = string.length_in_utf16_code_units() as f64;

        // 4. Let intStart be ? ToIntegerOrInfinity(start).
        let mut int_start = start.to_integer_or_infinity(vm)?;

        // 5. If intStart = -∞, let from be 0.
        if int_start == f64::NEG_INFINITY {
            int_start = 0.0;
        }
        // 6. Else if intStart < 0, let from be max(len + intStart, 0).
        else if int_start < 0.0 {
            int_start = (string_length + int_start).max(0.0);
        }
        // 7. Else, let from be min(intStart, len).
        else {
            int_start = int_start.min(string_length);
        }

        // 8. If end is undefined, let intEnd be len; else let intEnd be ? ToIntegerOrInfinity(end).
        let mut int_end = string_length;
        if !end.is_undefined() {
            int_end = end.to_integer_or_infinity(vm)?;
            // 9. If intEnd = -∞, let to be 0.
            if int_end == f64::NEG_INFINITY {
                int_end = 0.0;
            }
            // 10. Else if intEnd < 0, let to be max(len + intEnd, 0).
            else if int_end < 0.0 {
                int_end = (string_length + int_end).max(0.0);
            }
            // 11. Else, let to be min(intEnd, len).
            else {
                int_end = int_end.min(string_length);
            }
        }

        // 12. If from ≥ to, return the empty String.
        if int_start >= int_end {
            return Ok(empty_string_value(vm));
        }

        // 13. Return the substring of S from from to to.
        Ok(substring_value(
            vm,
            string,
            int_start as usize,
            (int_end - int_start) as usize,
        ))
    }

    // 22.1.3.23 String.prototype.split ( separator, limit ), https://tc39.es/ecma262/#sec-string.prototype.split
    fn split(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");
        let separator_argument = vm.argument(0);
        let limit_argument = vm.argument(1);

        // 1. Let thisValue be the this value.
        let this_value = vm.this_value();

        // 2. Perform ? RequireObjectCoercible(thisValue).
        require_object_coercible(vm, this_value)?;

        // 3. If separator is an Object, then
        if separator_argument.is_object() {
            // a. Let splitter be ? GetMethod(separator, @@split).
            let splitter = separator_argument.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().split),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::StringPrototypeSplitSplitter),
            )?;
            // b. If splitter is not undefined, then
            if let Some(splitter) = splitter {
                if splitter.builtin() == Some(Builtin::RegExpPrototypeSplit) && splitter.realm() == vm.current_realm() {
                    // OPTIMIZATION: The common case of RegExp.prototype[@@split]
                    this_value.to_primitive_string(vm)?;
                    unimplemented_runtime_function(
                        "RegExpPrototype::symbol_split_impl in String.prototype.split, which needs the RegExp builtins",
                        0,
                    );
                }
                // i. Return ? Call(splitter, separator, « thisValue, limit »).
                return call_function_object(vm, splitter, separator_argument, &[this_value, limit_argument]);
            }
        }

        // 4. Let str be ? ToString(thisValue).
        let string = this_value.to_primitive_string(vm)?;

        // 12. Let substrings be a new empty List.
        let array = Array::create(vm, realm, 0, None).must();
        let mut array_length = 0;
        let append = |index: usize, value: Gc<PrimitiveString>| {
            array
                .create_data_property_or_throw(vm, &PropertyKey::from_number(index as u64), Value::from_string(value))
                .must();
        };

        // 5. If limit is undefined, let lim be 232 - 1; else let lim be ℝ(? ToUint32(limit)).
        let mut limit = u32::MAX;
        if !limit_argument.is_undefined() {
            limit = limit_argument.to_u32(vm)?;
        }

        // 6. Let separatorStr be ? ToString(separator).
        let separator = separator_argument.to_utf16_string(vm)?;

        // 7. If lim = 0, then
        if limit == 0 {
            // a. Return CreateArrayFromList(« »).
            return Ok(Value::from_object(array.upcast::<Object>()));
        }

        let string_length = string.length_in_utf16_code_units();

        // 8. If separator is undefined, then
        if separator_argument.is_undefined() {
            // a. Return CreateArrayFromList(« S »).
            append(0, string);
            return Ok(Value::from_object(array.upcast::<Object>()));
        }

        // 9. Let separatorLength be the length of separatorStr.
        let separator_view = Utf16View::of_string(&separator);
        let separator_length = separator_view.length_in_code_units();

        // 10. If separatorLength = 0, then
        if separator_length == 0 {
            // a. Let strLen be the length of str.
            // NB: Declared above
            // b. Let outLen be the result of clamping lim between 0 and strLen.
            let out_length = (limit as usize).min(string_length);
            // c. Let head be the substring of str from 0 to outLen.
            // d. Let codeUnits be a List consisting of the sequence of code units that are the elements of head.
            // e. Return CreateArrayFromList(codeUnits).
            for index in 0..out_length {
                append(index, PrimitiveString::create_from_substring(vm, string, index, 1));
            }
            return Ok(Value::from_object(array.upcast::<Object>()));
        }

        // 11. If str is the empty String, return CreateArrayFromList(« str »).
        if string_length == 0 {
            append(0, string);
            return Ok(Value::from_object(array.upcast::<Object>()));
        }

        let string_data = string.utf16_string();
        let string_view = Utf16View::of_string(&string_data);

        // 13. Let searchStart be 0.
        let mut search_start = 0;

        // 14. Let matchIndex be StringIndexOf(str, separatorStr, 0).
        let mut match_index = string_view.find_code_unit_offset(separator_view, search_start);

        // 15. Repeat, while matchIndex is not not-found
        while let Some(index) = match_index {
            // a. Let substring be the substring of str from searchStart to matchIndex.
            // b. Append substring to substrings.
            append(
                array_length,
                PrimitiveString::create_from_substring(vm, string, search_start, index - search_start),
            );
            array_length += 1;

            // c. If the number of elements in substrings is lim, return CreateArrayFromList(substrings).
            if array_length == limit as usize {
                return Ok(Value::from_object(array.upcast::<Object>()));
            }

            // d. Set searchStart to matchIndex + separatorLength.
            search_start = index + separator_length;

            // e. Set matchIndex to StringIndexOf(str, separatorStr, searchStart).
            match_index = string_view.find_code_unit_offset(separator_view, search_start);
        }

        // 16. Let substring be the substring of str from searchStart.
        // 17. Append substring to substrings.
        append(
            array_length,
            PrimitiveString::create_from_substring(vm, string, search_start, string_length - search_start),
        );

        // 18. Return CreateArrayFromList(substrings).
        Ok(Value::from_object(array.upcast::<Object>()))
    }

    // 22.1.3.24 String.prototype.startsWith ( searchString [ , position ] ), https://tc39.es/ecma262/#sec-string.prototype.startswith
    fn starts_with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_string_value = vm.argument(0);
        let position = vm.argument(1);

        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let isRegExp be ? IsRegExp(searchString).
        let is_regexp = search_string_value.is_regexp(vm)?;

        // 4. If isRegExp is true, throw a TypeError exception.
        if is_regexp {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IsNotA,
                &[&"searchString", &"string, but a regular expression"],
            );
        }

        // 5. Let searchStr be ? ToString(searchString).
        let search_string = search_string_value.to_utf16_string(vm)?;

        // 6. Let len be the length of S.
        let string_length = string.length_in_utf16_code_units();

        let mut start = 0;

        // 7. If position is undefined, let pos be 0; else let pos be ? ToIntegerOrInfinity(position).
        if !position.is_undefined() {
            let pos = position.to_integer_or_infinity(vm)?;

            // 8. Let start be the result of clamping pos between 0 and len.
            start = clamp_to_index(pos, string_length);
        }

        // 9. Let searchLength be the length of searchStr.
        let search_view = Utf16View::of_string(&search_string);
        let search_length = search_view.length_in_code_units();

        // 10. If searchLength = 0, return true.
        if search_length == 0 {
            return Ok(Value::TRUE);
        }

        // 11. Let end be start + searchLength.
        let end = start + search_length;

        // 12. If end > len, return false.
        if end > string_length {
            return Ok(Value::FALSE);
        }

        // 13. Let substring be the substring of S from start to end.
        let substring_view = string.utf16_string_view().substring_view(start, end - start);

        // 14. If substring is searchStr, return true.
        // 15. Return false.
        Ok(Value::from_bool(substring_view == search_view))
    }

    // 22.1.3.25 String.prototype.substring ( start, end ), https://tc39.es/ecma262/#sec-string.prototype.substring
    fn substring(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let len be the length of S.
        let string_length = string.length_in_utf16_code_units();

        // 4. Let intStart be ? ToIntegerOrInfinity(start).
        let start = vm.argument(0).to_integer_or_infinity(vm)?;

        // 5. If end is undefined, let intEnd be len; else let intEnd be ? ToIntegerOrInfinity(end).
        let mut end = string_length as f64;
        if !vm.argument(1).is_undefined() {
            end = vm.argument(1).to_integer_or_infinity(vm)?;
        }

        // 6. Let finalStart be the result of clamping intStart between 0 and len.
        let final_start = clamp_to_index(start, string_length);

        // 7. Let finalEnd be the result of clamping intEnd between 0 and len.
        let final_end = clamp_to_index(end, string_length);

        // 8. Let from be min(finalStart, finalEnd).
        let from = final_start.min(final_end);

        // 9. Let to be max(finalStart, finalEnd).
        let to = final_start.max(final_end);

        // 10. Return the substring of S from from to to.
        Ok(substring_value(vm, string, from, to - from))
    }

    // 22.1.3.26 String.prototype.toLocaleLowerCase ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-string.prototype.tolocalelowercase
    // 20.1.2 String.prototype.toLocaleLowerCase ( [ locales ] ), https://tc39.es/ecma402/#sup-string.prototype.tolocalelowercase
    fn to_locale_lowercase(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);

        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Return ? TransformCase(S, locales, lower).
        let transformed = transform_case(vm, &string.utf16_string(), locales, TargetCase::Lower)?;
        Ok(string_value(vm, transformed))
    }

    // 22.1.3.27 String.prototype.toLocaleUpperCase ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-string.prototype.tolocaleuppercase
    // 20.1.3 String.prototype.toLocaleUpperCase ( [ locales ] ), https://tc39.es/ecma402/#sup-string.prototype.tolocaleuppercase
    fn to_locale_uppercase(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);

        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Return ? TransformCase(S, locales, upper).
        let transformed = transform_case(vm, &string.utf16_string(), locales, TargetCase::Upper)?;
        Ok(string_value(vm, transformed))
    }

    // 22.1.3.28 String.prototype.toLowerCase ( ), https://tc39.es/ecma262/#sec-string.prototype.tolowercase
    fn to_lowercase(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        // 3. Let sText be StringToCodePoints(S).
        let string = primitive_string_from(vm)?;

        // 4. Let lowerText be the result of toLowercase(sText), according to the Unicode Default Case Conversion algorithm.
        let lowercase = Utf16View::of_string(&string.utf16_string()).to_lowercase();

        // 5. Let L be CodePointsToString(lowerText).
        // 6. Return L.
        Ok(string_value(vm, lowercase))
    }

    // 22.1.3.29 String.prototype.toString ( ), https://tc39.es/ecma262/#sec-string.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? thisStringValue(this value).
        Ok(Value::from_string(this_string_value(vm, vm.this_value())?))
    }

    // 22.1.3.30 String.prototype.toUpperCase ( ), https://tc39.es/ecma262/#sec-string.prototype.touppercase
    fn to_uppercase(vm: &Vm) -> ThrowCompletionOr<Value> {
        // This method interprets a String value as a sequence of UTF-16 encoded code points, as described in 6.1.4.
        // It behaves in exactly the same way as String.prototype.toLowerCase, except that the String is mapped using the toUppercase algorithm of the Unicode Default Case Conversion.
        let string = primitive_string_from(vm)?;
        let uppercase = Utf16View::of_string(&string.utf16_string()).to_uppercase();
        Ok(string_value(vm, uppercase))
    }

    // 22.1.3.31 String.prototype.toWellFormed ( ), https://tc39.es/ecma262/#sec-string.prototype.towellformed
    fn to_well_formed(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let strLen be the length of S.
        // 4. Let k be 0.
        // 5. Let result be the empty String.
        // 6. Repeat, while k < strLen,
        //     a. Let cp be CodePointAt(S, k).
        //     b. If cp.[[IsUnpairedSurrogate]] is true, then
        //         i. Set result to the string-concatenation of result and 0xFFFD (REPLACEMENT CHARACTER).
        //     c. Else,
        //         i. Set result to the string-concatenation of result and UTF16EncodeCodePoint(cp.[[CodePoint]]).
        //     d. Set k to k + cp.[[CodeUnitCount]].
        Ok(string_value(vm, to_well_formed(&string.utf16_string())))
    }

    // 22.1.3.32 String.prototype.trim ( ), https://tc39.es/ecma262/#sec-string.prototype.trim
    fn trim(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? TrimString(S, start+end).
        Ok(string_value(vm, trim_string(vm, vm.this_value(), TrimMode::Both)?))
    }

    // 22.1.3.33 String.prototype.trimEnd ( ), https://tc39.es/ecma262/#sec-string.prototype.trimend
    fn trim_end(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? TrimString(S, end).
        Ok(string_value(vm, trim_string(vm, vm.this_value(), TrimMode::Right)?))
    }

    // 22.1.3.34 String.prototype.trimStart ( ), https://tc39.es/ecma262/#sec-string.prototype.trimstart
    fn trim_start(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? TrimString(S, start).
        Ok(string_value(vm, trim_string(vm, vm.this_value(), TrimMode::Left)?))
    }

    // 22.1.3.35 String.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-string.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(Value::from_string(this_string_value(vm, vm.this_value())?))
    }

    // 22.1.3.36 String.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-string.prototype-@@iterator
    fn symbol_iterator(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");

        // 1. Let O be ? RequireObjectCoercible(this value).
        let this_object = require_object_coercible(vm, vm.this_value())?;

        // 2. Let s be ? ToString(O).
        let string = this_object.to_utf16_string(vm)?;

        // 3. Let closure be a new Abstract Closure with no parameters that captures s and performs the following steps when called:
        //     ...
        // 4. Return CreateIteratorFromClosure(closure, "%StringIteratorPrototype%", %StringIteratorPrototype%).
        Ok(Value::from_object(
            StringIterator::create(vm, realm, string).upcast::<Object>(),
        ))
    }

    // B.2.2.1 String.prototype.substr ( start, length ), https://tc39.es/ecma262/#sec-string.prototype.substr
    fn substr(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? RequireObjectCoercible(this value).
        // 2. Let S be ? ToString(O).
        let string = primitive_string_from(vm)?;

        // 3. Let size be the length of S.
        let size = string.length_in_utf16_code_units();
        let size_f64 = size as f64;

        // 4. Let intStart be ? ToIntegerOrInfinity(start).
        let mut int_start = vm.argument(0).to_integer_or_infinity(vm)?;

        // 5. If intStart is -∞, set intStart to 0.
        if int_start == f64::NEG_INFINITY {
            int_start = 0.0;
        }
        // 6. Else if intStart < 0, set intStart to max(size + intStart, 0).
        else if int_start < 0.0 {
            int_start = (size_f64 + int_start).max(0.0);
        }
        // 7. Else, set intStart to min(intStart, size).
        else {
            int_start = int_start.min(size_f64);
        }

        // 8. If length is undefined, let intLength be size; otherwise let intLength be ? ToIntegerOrInfinity(length).
        let length = vm.argument(1);
        let mut int_length = if length.is_undefined() {
            size_f64
        } else {
            length.to_integer_or_infinity(vm)?
        };

        // 9. Set intLength to the result of clamping intLength between 0 and size.
        int_length = int_length.clamp(0.0, size_f64);

        // 10. Let intEnd be min(intStart + intLength, size).
        let int_end = ((int_start + int_length) as usize).min(size);

        if int_start >= int_end as f64 {
            return Ok(empty_string_value(vm));
        }

        // 11. Return the substring of S from intStart to intEnd.
        let int_start = int_start as usize;
        Ok(substring_value(vm, string, int_start, int_end - int_start))
    }

    // B.2.2.2 String.prototype.anchor ( name ), https://tc39.es/ecma262/#sec-string.prototype.anchor
    fn anchor(vm: &Vm) -> ThrowCompletionOr<Value> {
        let name = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "a", "name", name).
        create_html(vm, vm.this_value(), "a", "name", name)
    }

    // B.2.2.3 String.prototype.big ( ), https://tc39.es/ecma262/#sec-string.prototype.big
    fn big(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "big", "", "").
        create_html(vm, vm.this_value(), "big", "", Value::UNDEFINED)
    }

    // B.2.2.4 String.prototype.blink ( ), https://tc39.es/ecma262/#sec-string.prototype.blink
    fn blink(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "blink", "", "").
        create_html(vm, vm.this_value(), "blink", "", Value::UNDEFINED)
    }

    // B.2.2.5 String.prototype.bold ( ), https://tc39.es/ecma262/#sec-string.prototype.bold
    fn bold(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "b", "", "").
        create_html(vm, vm.this_value(), "b", "", Value::UNDEFINED)
    }

    // B.2.2.6 String.prototype.fixed ( ), https://tc39.es/ecma262/#sec-string.prototype.fixed
    fn fixed(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "tt", "", "").
        create_html(vm, vm.this_value(), "tt", "", Value::UNDEFINED)
    }

    // B.2.2.7 String.prototype.fontcolor ( color ), https://tc39.es/ecma262/#sec-string.prototype.fontcolor
    fn fontcolor(vm: &Vm) -> ThrowCompletionOr<Value> {
        let color = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "font", "color", color).
        create_html(vm, vm.this_value(), "font", "color", color)
    }

    // B.2.2.8 String.prototype.fontsize ( size ), https://tc39.es/ecma262/#sec-string.prototype.fontsize
    fn fontsize(vm: &Vm) -> ThrowCompletionOr<Value> {
        let size = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "font", "size", size).
        create_html(vm, vm.this_value(), "font", "size", size)
    }

    // B.2.2.9 String.prototype.italics ( ), https://tc39.es/ecma262/#sec-string.prototype.italics
    fn italics(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "i", "", "").
        create_html(vm, vm.this_value(), "i", "", Value::UNDEFINED)
    }

    // B.2.2.10 String.prototype.link ( url ), https://tc39.es/ecma262/#sec-string.prototype.link
    fn link(vm: &Vm) -> ThrowCompletionOr<Value> {
        let url = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "a", "href", url).
        create_html(vm, vm.this_value(), "a", "href", url)
    }

    // B.2.2.11 String.prototype.small ( ), https://tc39.es/ecma262/#sec-string.prototype.small
    fn small(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "small", "", "").
        create_html(vm, vm.this_value(), "small", "", Value::UNDEFINED)
    }

    // B.2.2.12 String.prototype.strike ( ), https://tc39.es/ecma262/#sec-string.prototype.strike
    fn strike(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "strike", "", "").
        create_html(vm, vm.this_value(), "strike", "", Value::UNDEFINED)
    }

    // B.2.2.13 String.prototype.sub ( ), https://tc39.es/ecma262/#sec-string.prototype.sub
    fn sub(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "sub", "", "").
        create_html(vm, vm.this_value(), "sub", "", Value::UNDEFINED)
    }

    // B.2.2.14 String.prototype.sup ( ), https://tc39.es/ecma262/#sec-string.prototype.sup
    fn sup(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Return ? CreateHTML(S, "sup", "", "").
        create_html(vm, vm.this_value(), "sup", "", Value::UNDEFINED)
    }
}

// thisStringValue ( value ), https://tc39.es/ecma262/#thisstringvalue
fn this_string_value(vm: &Vm, value: Value) -> ThrowCompletionOr<Gc<PrimitiveString>> {
    // 1. If value is a String, return value.
    if value.is_string() {
        return Ok(value.as_string());
    }

    // 2. If value is an Object and value has a [[StringData]] internal slot, then
    if value.is_object()
        && let Some(string) = value.as_object().downcast::<StringObject>()
    {
        // a. Let s be value.[[StringData]].
        // b. Assert: s is a String.
        // c. Return s.
        return Ok(string.primitive_string());
    }

    // 3. Throw a TypeError exception.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"String"])
}

/// RegExpCreate followed by the Invoke of `method_name` on the new RegExp object, which the String.prototype methods
/// that take a regular expression perform when it is not an object with a method of that name.
fn regexp_create_and_invoke(
    _vm: &Vm,
    _pattern: Value,
    _flags: Value,
    _string: Gc<PrimitiveString>,
    method_name: &str,
) -> ThrowCompletionOr<Value> {
    unimplemented_runtime_function(
        &format!(
            "RegExpCreate and the Invoke of RegExp.prototype[{method_name}] in String.prototype, which need the \
             RegExp builtins"
        ),
        0,
    )
}

#[rustfmt::skip]
const ASCII_COLLATION_PRIMARY_WEIGHTS: [u8; 128] = [
    0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    6, 12, 16, 28, 38, 29, 27, 15, 17, 18, 24, 32,  9,  8, 14, 25,
   39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 11, 10, 33, 34, 35, 13,
   23, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
   64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 19, 26, 20, 31,  7,
   30, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
   64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 21, 36, 22, 37,  0,
];

#[rustfmt::skip]
const ASCII_COLLATION_TERTIARY_WEIGHTS: [u8; 128] = [
    0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,
];

fn try_fast_ascii_string_compare(a: &[u8], b: &[u8]) -> Option<i32> {
    let length = a.len().min(b.len());
    let ordering = |less: bool| if less { -1 } else { 1 };
    let mut tertiary_result = 0;

    for i in 0..length {
        let a_character = a[i];
        let b_character = b[i];

        if a_character != b_character {
            let a_primary_weight = ASCII_COLLATION_PRIMARY_WEIGHTS[usize::from(a_character)];
            // We bail for control characters so that we don't have to compare secondary weights.
            if a_primary_weight == 0 {
                return None;
            }
            let b_primary_weight = ASCII_COLLATION_PRIMARY_WEIGHTS[usize::from(b_character)];
            if b_primary_weight == 0 {
                return None;
            }
            if a_primary_weight != b_primary_weight {
                return Some(ordering(a_primary_weight < b_primary_weight));
            }
            if tertiary_result == 0 {
                let a_tertiary_weight = ASCII_COLLATION_TERTIARY_WEIGHTS[usize::from(a_character)];
                let b_tertiary_weight = ASCII_COLLATION_TERTIARY_WEIGHTS[usize::from(b_character)];
                if a_tertiary_weight != b_tertiary_weight {
                    tertiary_result = ordering(a_tertiary_weight < b_tertiary_weight);
                }
            }
        }
    }

    let longer = if a.len() > b.len() { a } else { b };
    for &character in &longer[length..] {
        if ASCII_COLLATION_PRIMARY_WEIGHTS[usize::from(character)] != 0 {
            return Some(ordering(a.len() < b.len()));
        }
    }

    Some(tertiary_result)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PadPlacement {
    Start,
    End,
}

// 22.1.3.17.1 StringPad ( O, maxLength, fillString, placement ), https://tc39.es/ecma262/#sec-stringpad
fn pad_string(
    vm: &Vm,
    string: Gc<PrimitiveString>,
    max_length: Value,
    fill_string: Value,
    placement: PadPlacement,
) -> ThrowCompletionOr<Value> {
    // 1. Let S be ? ToString(O).

    // 2. Let intMaxLength be ℝ(? ToLength(maxLength)).
    let int_max_length = max_length.to_length(vm)?;

    // 3. Let stringLength be the length of S.
    let string_length = string.length_in_utf16_code_units() as u64;

    // 4. If intMaxLength ≤ stringLength, return S.
    if int_max_length <= string_length {
        return Ok(Value::from_string(string));
    }

    // 5. If fillString is undefined, let filler be the String value consisting solely of the code unit 0x0020 (SPACE).
    let mut filler = Utf16String::from_utf8(" ");
    if !fill_string.is_undefined() {
        // 6. Else, let filler be ? ToString(fillString).
        filler = fill_string.to_utf16_string(vm)?;

        // 7. If filler is the empty String, return S.
        if filler.is_empty() {
            return Ok(Value::from_string(string));
        }
    }

    // 8. Let fillLen be intMaxLength - stringLength.
    let fill_length = (int_max_length - string_length) as usize;

    checked_js_string_length_sum(
        vm,
        string_length as usize,
        fill_length,
        ErrorType::StringSizeMustNotOverflow,
    )?;

    let filler_view = Utf16View::of_string(&filler);
    let fill_code_units = filler_view.length_in_code_units();
    let string = string.utf16_string();
    let mut builder = Utf16StringBuilder::new();
    if placement == PadPlacement::End {
        builder.append(Utf16View::of_string(&string));
    }

    // 9. Let truncatedStringFiller be the String value consisting of repeated concatenations of filler truncated to length fillLen.
    builder.append_repeated(filler_view, fill_length / fill_code_units);
    builder.append(filler_view.substring_view(0, fill_length % fill_code_units));

    // 10. If placement is start, return the string-concatenation of truncatedStringFiller and S.
    // 11. Else, return the string-concatenation of S and truncatedStringFiller.
    if placement == PadPlacement::Start {
        builder.append(Utf16View::of_string(&string));
    }
    Ok(string_value(vm, builder.to_utf16_string()))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TargetCase {
    Lower,
    Upper,
}

// 20.1.2.1 TransformCase ( S, locales, targetCase ), https://tc39.es/ecma402/#sec-transform-case
#[allow(
    clippy::unnecessary_wraps,
    reason = "CanonicalizeLocaleList throws once the Intl abstract operations exist"
)]
fn transform_case(
    _vm: &Vm,
    string: &Utf16String,
    locales: Value,
    target_case: TargetCase,
) -> ThrowCompletionOr<Utf16String> {
    // 1. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    if !locales.is_undefined() {
        unimplemented_runtime_function(
            "Intl::canonicalize_locale_list of a locales argument that is not undefined in TransformCase, which \
             needs the Intl abstract operations",
            0,
        );
    }

    // 2. If requestedLocales is not an empty List, then
    //     a. Let requestedLocale be requestedLocales[0].
    // 3. Else,
    //     a. Let requestedLocale be ! DefaultLocale().
    // 4. Let availableLocales be an Available Locales List which includes the language tags for which the Unicode Character Database contains language-sensitive case mappings. If the implementation supports additional locale-sensitive case mappings, availableLocales should also include their corresponding language tags.
    // 5. Let match be LookupMatchingLocaleByPrefix(availableLocales, « requestedLocale »).
    // 6. If match is not undefined, let locale be match.[[locale]]; else let locale be "und".
    // NB: An undefined locales argument is the empty list, so the locale is LibUnicode's default locale, "en", which
    //     has no language-sensitive case mappings.

    // 7. Let codePoints be StringToCodePoints(S).
    let string = Utf16View::of_string(string);

    let new_code_points = match target_case {
        // 8. If targetCase is lower, then
        TargetCase::Lower => {
            // a. Let newCodePoints be a List whose elements are the result of a lowercase transformation of codePoints according to an implementation-derived algorithm using locale or the Unicode Default Case Conversion algorithm.
            string.to_lowercase()
        }
        // 9. Else,
        TargetCase::Upper => {
            // a. Assert: targetCase is upper.
            // b. Let newCodePoints be a List whose elements are the result of an uppercase transformation of codePoints according to an implementation-derived algorithm using locale or the Unicode Default Case Conversion algorithm.
            string.to_uppercase()
        }
    };

    // 10. Return CodePointsToString(newCodePoints).
    Ok(new_code_points)
}

// 22.1.3.32.1 TrimString ( string, where ), https://tc39.es/ecma262/#sec-trimstring
pub fn trim_string(vm: &Vm, input_value: Value, where_: TrimMode) -> ThrowCompletionOr<Utf16String> {
    // 1. Let str be ? RequireObjectCoercible(string).
    let input_string = require_object_coercible(vm, input_value)?;

    // 2. Let S be ? ToString(str).
    let string = input_string.to_utf16_string(vm)?;

    // 3. If where is start, let T be the String value that is a copy of S with leading white space removed.
    // 4. Else if where is end, let T be the String value that is a copy of S with trailing white space removed.
    // 5. Else,
    // a. Assert: where is start+end.
    // b. Let T be the String value that is a copy of S with both leading and trailing white space removed.
    let string_view = Utf16View::of_string(&string);
    let trimmed_string = string_view.trim(&WHITESPACE_CHARACTER_CODE_UNITS, where_);

    // 6. Return T.
    if trimmed_string.length_in_code_units() == string_view.length_in_code_units() {
        return Ok(string);
    }
    Ok(trimmed_string.to_utf16_string())
}

// B.2.2.2.1 CreateHTML ( string, tag, attribute, value ), https://tc39.es/ecma262/#sec-createhtml
fn create_html(vm: &Vm, string: Value, tag: &str, attribute: &str, value: Value) -> ThrowCompletionOr<Value> {
    // 1. Let str be ? RequireObjectCoercible(string).
    require_object_coercible(vm, string)?;

    // 2. Let S be ? ToString(str).
    let str = string.to_utf16_string(vm)?;

    // 3. Let p1 be the string-concatenation of "<" and tag.
    let mut builder = Utf16StringBuilder::new();
    builder.append_ascii("<");
    builder.append_ascii(tag);

    // 4. If attribute is not the empty String, then
    if !attribute.is_empty() {
        // a. Let V be ? ToString(value).
        let value_string = value.to_utf16_string(vm)?;

        // b. Let escapedV be the String value that is the same as V except that each occurrence of the code unit 0x0022 (QUOTATION MARK) in V has been replaced with the six code unit sequence "&quot;".
        // c. Set p1 to the string-concatenation of:
        // - p1
        // - the code unit 0x0020 (SPACE)
        builder.append_ascii(" ");
        // - attribute
        builder.append_ascii(attribute);
        // - the code unit 0x003D (EQUALS SIGN)
        // - the code unit 0x0022 (QUOTATION MARK)
        builder.append_ascii("=\"");
        // - escapedV
        for code_unit in Utf16View::of_string(&value_string).code_units() {
            if code_unit == u16::from(b'"') {
                builder.append_ascii("&quot;");
            } else {
                builder.append_code_unit(code_unit);
            }
        }
        // - the code unit 0x0022 (QUOTATION MARK)
        builder.append_ascii("\"");
    }

    // 5. Let p2 be the string-concatenation of p1 and ">".
    builder.append_ascii(">");

    // 6. Let p3 be the string-concatenation of p2 and S.
    builder.append(Utf16View::of_string(&str));

    // 7. Let p4 be the string-concatenation of p3, "</", tag, and ">".
    builder.append_ascii("</");
    builder.append_ascii(tag);
    builder.append_ascii(">");

    // 8. Return p4.
    Ok(string_value(vm, builder.to_utf16_string()))
}
