/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, RawNativeFunction, define_native_function_class, raw_native};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::regexp_legacy_static_properties::{
    RegExpLegacyStaticProperties, get_legacy_regexp_static_property, set_legacy_regexp_static_property,
};
use crate::runtime::regexp_object::{RegExpObject, regexp_alloc};
use crate::runtime::string_prototype::code_point_at;
use crate::runtime::value::same_value;
use crate::utf16::{Utf16StringBuilder, Utf16View, is_unicode_surrogate};

fn is_syntax_character(code_point: u32) -> bool {
    const SYNTAX_CHARACTERS: &[u8] = b"^$\\.*+?()[]{}|";
    code_point < 0x80 && SYNTAX_CHARACTERS.contains(&(code_point as u8))
}

fn is_ascii_space(code_point: u32) -> bool {
    matches!(code_point, 0x20 | 0x09 | 0x0a | 0x0b | 0x0c | 0x0d)
}

fn is_whitespace(code_point: u32) -> bool {
    if is_ascii_space(code_point) {
        return true;
    }
    if code_point == 0x00A0 || code_point == 0xFEFF {
        return true;
    }
    libunicode_rust::character_types::code_point_has_space_separator_general_category(code_point)
}

fn is_line_terminator(code_point: u32) -> bool {
    code_point == u32::from(b'\n') || code_point == u32::from(b'\r') || code_point == 0x2028 || code_point == 0x2029
}

fn is_ascii_alphanumeric(code_point: u32) -> bool {
    code_point < 0x80 && (code_point as u8).is_ascii_alphanumeric()
}

/// Whether `function` is a RawNativeFunction that runs `native_function`, the C++ comparison of
/// RawNativeFunction::native_function() with a native function's address.
pub fn is_raw_native_function_running(
    vm: &Vm,
    function: Gc<FunctionObject>,
    native_function: RawNativeFunctionPointer,
) -> bool {
    let Some(raw_native_function) = function.downcast::<RawNativeFunction>() else {
        return false;
    };
    match (raw_native_function.native_function(vm), native_function) {
        (Some(function_pointer), Some(expected_function_pointer)) => {
            core::ptr::fn_addr_eq(function_pointer, expected_function_pointer)
        }
        _ => false,
    }
}

/// The getter of RegExp[@@species], which has_intrinsic_symbol_species_getter() looks for.
static SYMBOL_SPECIES_GETTER: RawNativeFunctionPointer = raw_native!(RegExpConstructor::symbol_species_getter);

#[repr(C)]
#[derive(Trace)]
pub struct RegExpConstructor {
    base: NativeFunction,
    legacy_static_properties: RegExpLegacyStaticProperties,
}

define_native_function_class!(
    RegExpConstructor,
    initialize: RegExpConstructor::initialize,
    call: RegExpConstructor::call,
    construct: RegExpConstructor::construct
);

impl RegExpConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<RegExpConstructor> {
        realm.create_object(
            vm,
            RegExpConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.RegExp.as_string().clone(),
                    realm.function_prototype(),
                ),
                legacy_static_properties: RegExpLegacyStaticProperties::default(),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 22.2.5.2 RegExp.prototype, https://tc39.es/ecma262/#sec-regexp.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().regexp_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.escape,
            raw_native!(RegExpConstructor::escape),
            1,
            attributes,
            None,
        );
        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            SYMBOL_SPECIES_GETTER,
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        // Additional properties of the RegExp constructor, https://github.com/tc39/proposal-regexp-legacy-features#additional-properties-of-the-regexp-constructor
        let configurable = PropertyAttributes::new(Attribute::CONFIGURABLE);
        let legacy_accessors: [(&PropertyKey, RawNativeFunctionPointer, RawNativeFunctionPointer); 19] = [
            (
                &names.input,
                raw_native!(RegExpConstructor::input_getter),
                raw_native!(RegExpConstructor::input_setter),
            ),
            (
                &names.inputAlias,
                raw_native!(RegExpConstructor::input_alias_getter),
                raw_native!(RegExpConstructor::input_alias_setter),
            ),
            (
                &names.lastMatch,
                raw_native!(RegExpConstructor::last_match_getter),
                None,
            ),
            (
                &names.lastMatchAlias,
                raw_native!(RegExpConstructor::last_match_alias_getter),
                None,
            ),
            (
                &names.lastParen,
                raw_native!(RegExpConstructor::last_paren_getter),
                None,
            ),
            (
                &names.lastParenAlias,
                raw_native!(RegExpConstructor::last_paren_alias_getter),
                None,
            ),
            (
                &names.leftContext,
                raw_native!(RegExpConstructor::left_context_getter),
                None,
            ),
            (
                &names.leftContextAlias,
                raw_native!(RegExpConstructor::left_context_alias_getter),
                None,
            ),
            (
                &names.rightContext,
                raw_native!(RegExpConstructor::right_context_getter),
                None,
            ),
            (
                &names.rightContextAlias,
                raw_native!(RegExpConstructor::right_context_alias_getter),
                None,
            ),
            (&names.dollar_1, raw_native!(RegExpConstructor::group_1_getter), None),
            (&names.dollar_2, raw_native!(RegExpConstructor::group_2_getter), None),
            (&names.dollar_3, raw_native!(RegExpConstructor::group_3_getter), None),
            (&names.dollar_4, raw_native!(RegExpConstructor::group_4_getter), None),
            (&names.dollar_5, raw_native!(RegExpConstructor::group_5_getter), None),
            (&names.dollar_6, raw_native!(RegExpConstructor::group_6_getter), None),
            (&names.dollar_7, raw_native!(RegExpConstructor::group_7_getter), None),
            (&names.dollar_8, raw_native!(RegExpConstructor::group_8_getter), None),
            (&names.dollar_9, raw_native!(RegExpConstructor::group_9_getter), None),
        ];
        for (property_key, getter, setter) in legacy_accessors {
            object.define_native_accessor(vm, realm, property_key, getter, setter, configurable);
        }
    }

    pub fn legacy_static_properties(&self) -> &RegExpLegacyStaticProperties {
        &self.legacy_static_properties
    }

    // 22.2.4.1 RegExp ( pattern, flags ), https://tc39.es/ecma262/#sec-regexp-pattern-flags
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        let pattern = vm.argument(0);
        let flags = vm.argument(1);

        // 1. Let patternIsRegExp be ? IsRegExp(pattern).
        let pattern_is_regexp = pattern.is_regexp(vm)?;

        // 2. If NewTarget is undefined, then
        // a. Let newTarget be the active function object.
        let new_target = function.as_function_object_gc();

        // b. If patternIsRegExp is true and flags is undefined, then
        if pattern_is_regexp && flags.is_undefined() {
            // i. Let patternConstructor be ? Get(pattern, "constructor").
            let pattern_constructor = pattern.as_object().get(vm, &vm.names.constructor)?;

            // ii. If SameValue(newTarget, patternConstructor) is true, return pattern.
            if same_value(Value::from_object(new_target), pattern_constructor) {
                return Ok(pattern);
            }
        }

        // Reuse the already-computed patternIsRegExp to avoid re-reading @@match.
        Ok(Value::from_object(Self::construct_impl(
            vm,
            new_target,
            pattern_is_regexp,
        )?))
    }

    // 22.2.4.1 RegExp ( pattern, flags ), https://tc39.es/ecma262/#sec-regexp-pattern-flags
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        // 1. Let patternIsRegExp be ? IsRegExp(pattern).
        let pattern_is_regexp = vm.argument(0).is_regexp(vm)?;

        // 3. Else, let newTarget be NewTarget.
        Self::construct_impl(vm, new_target, pattern_is_regexp)
    }

    pub fn construct_impl(
        vm: &Vm,
        new_target: Gc<FunctionObject>,
        pattern_is_regexp: bool,
    ) -> ThrowCompletionOr<Gc<Object>> {
        let pattern = vm.argument(0);
        let flags = vm.argument(1);

        let pattern_value;
        let flags_value;

        // 4. If pattern is an Object and pattern has a [[RegExpMatcher]] internal slot, then
        if let Some(regexp_pattern) = pattern
            .is_object()
            .then(|| pattern.as_object().downcast::<RegExpObject>())
            .flatten()
        {
            // a. Let P be pattern.[[OriginalSource]].
            pattern_value = Value::from_string(PrimitiveString::create(vm, regexp_pattern.pattern()));

            // b. If flags is undefined, let F be pattern.[[OriginalFlags]].
            if flags.is_undefined() {
                flags_value = Value::from_string(PrimitiveString::create(vm, regexp_pattern.flags()));
            }
            // c. Else, let F be flags.
            else {
                flags_value = flags;
            }
        }
        // 5. Else if patternIsRegExp is true, then
        else if pattern_is_regexp {
            // a. Let P be ? Get(pattern, "source").
            pattern_value = pattern.as_object().get(vm, &vm.names.source)?;

            // b. If flags is undefined, then
            if flags.is_undefined() {
                // i. Let F be ? Get(pattern, "flags").
                flags_value = pattern.as_object().get(vm, &vm.names.flags)?;
            }
            // c. Else, let F be flags.
            else {
                flags_value = flags;
            }
        }
        // 6. Else,
        else {
            // a. Let P be pattern.
            pattern_value = pattern;

            // b. Let F be flags.
            flags_value = flags;
        }

        // 7. Let O be ? RegExpAlloc(newTarget).
        let regexp_object = regexp_alloc(vm, new_target)?;

        // 8. Return ? RegExpInitialize(O, P, F).
        Ok(regexp_object
            .regexp_initialize(vm, pattern_value, flags_value)?
            .upcast())
    }

    // 22.2.5.1 RegExp.escape ( S ), https://tc39.es/ecma262/#sec-regexp.escape
    fn escape(vm: &Vm) -> ThrowCompletionOr<Value> {
        let string = vm.argument(0);

        // 1. If S is not a String, throw a TypeError exception.
        if !string.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&string]);
        }

        // 2. Let escaped be the empty String.
        let code_point_list = string.as_string().utf16_string();
        let code_point_list = Utf16View::of_string(&code_point_list);
        let mut escaped = Utf16StringBuilder::with_capacity(code_point_list.length_in_code_units());

        // 3. Let cpList be StringToCodePoints(S).

        // 4. For each code point c of cpList, do
        let mut position = 0;
        while position < code_point_list.length_in_code_units() {
            let code_point = code_point_at(code_point_list, position);
            position += code_point.code_unit_count;
            let code_point = code_point.code_point;

            // a. If escaped is the empty String and c is matched by either DecimalDigit or AsciiLetter, then
            if escaped.is_empty() && is_ascii_alphanumeric(code_point) {
                // i. NOTE: Escaping a leading digit ensures that output corresponds with pattern text which may be used
                //    after a \0 character escape or a DecimalEscape such as \1 and still match S rather than be interpreted
                //    as an extension of the preceding escape sequence. Escaping a leading ASCII letter does the same for
                //    the context after \c.

                // ii. Let numericValue be the numeric value of c.
                // iii. Let hex be Number::toString(𝔽(numericValue), 16).
                // iv. Assert: The length of hex is 2.
                // v. Set escaped to the string-concatenation of the code unit 0x005C (REVERSE SOLIDUS), "x", and hex.
                escaped.append_ascii(&format!("\\x{code_point:02x}"));
            }
            // b. Else,
            else {
                // i. Set escaped to the string-concatenation of escaped and EncodeForRegExpEscape(c).
                escaped.append(Utf16View::of_string(&encode_for_regexp_escape(code_point)));
            }
        }

        // 5. Return escaped.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            escaped.to_utf16_string(),
        )))
    }

    // 22.2.5.3 get RegExp [ %Symbol.species% ], https://tc39.es/ecma262/#sec-get-regexp-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }

    // Whether @@species still resolves to the intrinsic getter, which hands back its this value — so a
    // SpeciesConstructor that reaches this constructor would observe nothing and return %RegExp%.
    pub fn has_intrinsic_symbol_species_getter(&self, vm: &Vm) -> bool {
        let Some(species) = self.storage_get(vm, &PropertyKey::from(vm.well_known_symbols().species)) else {
            return false;
        };
        if !species.value.is_accessor() {
            return false;
        }

        let Some(getter) = species.value.as_accessor().getter() else {
            return false;
        };
        is_raw_native_function_running(vm, getter, SYMBOL_SPECIES_GETTER)
    }

    fn intrinsic_regexp_constructor(vm: &Vm) -> Gc<RegExpConstructor> {
        vm.current_realm()
            .expect("a builtin runs in a realm")
            .intrinsics()
            .regexp_constructor(vm)
    }

    // get RegExp.input, https://github.com/tc39/proposal-regexp-legacy-features#get-regexpinput
    fn input_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp_constructor = Self::intrinsic_regexp_constructor(vm);

        // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpInput]]).
        let property_getter = RegExpLegacyStaticProperties::input;
        get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter)
    }

    // get RegExp.$_, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp_
    fn input_alias_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // Keep the same implementation with `get RegExp.input`
        Self::input_getter(vm)
    }

    // set RegExp.input, https://github.com/tc39/proposal-regexp-legacy-features#set-regexpinput--val
    fn input_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp_constructor = Self::intrinsic_regexp_constructor(vm);

        // 1. Perform ? SetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpInput]], val).
        let property_setter = RegExpLegacyStaticProperties::set_input;
        set_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_setter, vm.argument(0))?;
        Ok(Value::UNDEFINED)
    }

    // set RegExp.$_, https://github.com/tc39/proposal-regexp-legacy-features#set-regexp_---val
    fn input_alias_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // Keep the same implementation with `set RegExp.input`
        Self::input_setter(vm)
    }

    // get RegExp.lastMatch, https://github.com/tc39/proposal-regexp-legacy-features#get-regexplastmatch
    fn last_match_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp_constructor = Self::intrinsic_regexp_constructor(vm);

        // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpLastMatch]]).
        let property_getter = RegExpLegacyStaticProperties::last_match;
        get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter)
    }

    // get RegExp.$&, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp
    fn last_match_alias_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // Keep the same implementation with `get RegExp.lastMatch`
        Self::last_match_getter(vm)
    }

    // get RegExp.lastParen, https://github.com/tc39/proposal-regexp-legacy-features#get-regexplastparen
    fn last_paren_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp_constructor = Self::intrinsic_regexp_constructor(vm);

        // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpLastParen]]).
        let property_getter = RegExpLegacyStaticProperties::last_paren;
        get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter)
    }

    // get RegExp.$+, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp-1
    fn last_paren_alias_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // Keep the same implementation with `get RegExp.lastParen`
        Self::last_paren_getter(vm)
    }

    // get RegExp.leftContext, https://github.com/tc39/proposal-regexp-legacy-features#get-regexpleftcontext
    fn left_context_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp_constructor = Self::intrinsic_regexp_constructor(vm);

        // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpLeftContext]]).
        let property_getter = RegExpLegacyStaticProperties::left_context;
        get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter)
    }

    // get RegExp.$`, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp-2
    fn left_context_alias_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // Keep the same implementation with `get RegExp.leftContext`
        Self::left_context_getter(vm)
    }

    // get RegExp.rightContext, https://github.com/tc39/proposal-regexp-legacy-features#get-regexprightcontext
    fn right_context_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let regexp_constructor = Self::intrinsic_regexp_constructor(vm);

        // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpRightContext]]).
        let property_getter = RegExpLegacyStaticProperties::right_context;
        get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter)
    }

    // get RegExp.$', https://github.com/tc39/proposal-regexp-legacy-features#get-regexp-3
    fn right_context_alias_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // Keep the same implementation with `get RegExp.rightContext`
        Self::right_context_getter(vm)
    }
}

macro_rules! define_regexp_group_getters {
    ($($getter:ident => $property_getter:ident;)*) => {
        impl RegExpConstructor {
            $(
                fn $getter(vm: &Vm) -> ThrowCompletionOr<Value> {
                    let regexp_constructor = Self::intrinsic_regexp_constructor(vm);

                    // 1. Return ? GetLegacyRegExpStaticProperty(%RegExp%, this value, [[RegExpParenN]]).
                    let property_getter = RegExpLegacyStaticProperties::$property_getter;
                    get_legacy_regexp_static_property(vm, regexp_constructor, vm.this_value(), property_getter)
                }
            )*
        }
    };
}

// get RegExp.$1, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp1
// get RegExp.$2, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp2
// get RegExp.$3, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp3
// get RegExp.$4, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp4
// get RegExp.$5, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp5
// get RegExp.$6, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp6
// get RegExp.$7, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp7
// get RegExp.$8, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp8
// get RegExp.$9, https://github.com/tc39/proposal-regexp-legacy-features#get-regexp9
define_regexp_group_getters! {
    group_1_getter => dollar_1;
    group_2_getter => dollar_2;
    group_3_getter => dollar_3;
    group_4_getter => dollar_4;
    group_5_getter => dollar_5;
    group_6_getter => dollar_6;
    group_7_getter => dollar_7;
    group_8_getter => dollar_8;
    group_9_getter => dollar_9;
}

// 22.2.5.1.1 EncodeForRegExpEscape ( cp ), https://tc39.es/ecma262/#sec-encodeforregexpescape
fn encode_for_regexp_escape(code_point: u32) -> Utf16String {
    // https://tc39.es/ecma262/#table-controlescape-code-point-values
    // Table 63: ControlEscape Code Point Values
    const CONTROL_ESCAPES: [(u32, char); 5] = [(0x09, 't'), (0x0A, 'n'), (0x0B, 'v'), (0x0C, 'f'), (0x0D, 'r')];

    // 1. If c is matched by SyntaxCharacter or c is U+002F (SOLIDUS), then
    if is_syntax_character(code_point) || code_point == u32::from(b'/') {
        // a. Return the string-concatenation of 0x005C (REVERSE SOLIDUS) and UTF16EncodeCodePoint(c).
        let mut builder = Utf16StringBuilder::new();
        builder.append_ascii("\\");
        builder.append_code_point(code_point);
        return builder.to_utf16_string();
    }

    // 2. Else if c is the code point listed in some cell of the “Code Point” column of Table 63, then
    if let Some((_, control_escape)) = CONTROL_ESCAPES
        .iter()
        .find(|(escape_code_point, _)| *escape_code_point == code_point)
    {
        // a. Return the string-concatenation of 0x005C (REVERSE SOLIDUS) and the string in the “ControlEscape” column
        //    of the row whose “Code Point” column contains c.
        return Utf16String::from_utf8(&format!("\\{control_escape}"));
    }

    // 3. Let otherPunctuators be the string-concatenation of ",-=<>#&!%:;@~'`" and the code unit 0x0022 (QUOTATION MARK).
    // 4. Let toEscape be StringToCodePoints(otherPunctuators).
    const TO_ESCAPE: &[u8] = b",-=<>#&!%:;@~'`\"";

    // 5. If toEscape contains c, c is matched by either WhiteSpace or LineTerminator, or c has the same numeric value
    //    as a leading surrogate or trailing surrogate, then
    if (code_point < 0x80 && TO_ESCAPE.contains(&(code_point as u8)))
        || is_whitespace(code_point)
        || is_line_terminator(code_point)
        || (code_point <= 0xFFFF && is_unicode_surrogate(code_point as u16))
    {
        // a. Let cNum be the numeric value of c.
        // b. If cNum ≤ 0xFF, then
        if code_point <= 0xFF {
            // i. Let hex be Number::toString(𝔽(cNum), 16).
            // ii. Return the string-concatenation of the code unit 0x005C (REVERSE SOLIDUS), "x", and
            //     StringPad(hex, 2, "0", START).
            return Utf16String::from_utf8(&format!("\\x{code_point:02x}"));
        }

        // c. Let escaped be the empty String.
        // d. Let codeUnits be UTF16EncodeCodePoint(c).
        // e. For each code unit cu of codeUnits, do
        //     i. Set escaped to the string-concatenation of escaped and UnicodeEscape(cu).
        // f. Return escaped.
        return Utf16String::from_utf8(&format!("\\u{code_point:04x}"));
    }

    // 6. Return UTF16EncodeCodePoint(c).
    let mut builder = Utf16StringBuilder::new();
    builder.append_code_point(code_point);
    builder.to_utf16_string()
}
