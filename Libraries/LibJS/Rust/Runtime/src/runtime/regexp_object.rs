/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;
use std::rc::Rc;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::ordinary_create_from_constructor_of;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::ecmascript_regex::{EcmaScriptCompileFlags, EcmaScriptRegex};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, ObjectMethods, ShouldThrowExceptions,
};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::realm::Realm;
use crate::runtime::value::same_value;
use crate::utf16::{Utf16StringBuilder, Utf16View};

/// RegExpObject::Flags.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RegExpFlags(u8);

impl RegExpFlags {
    pub const HAS_INDICES: Self = Self(1 << 0);
    pub const GLOBAL: Self = Self(1 << 1);
    pub const IGNORE_CASE: Self = Self(1 << 2);
    pub const MULTILINE: Self = Self(1 << 3);
    pub const DOT_ALL: Self = Self(1 << 4);
    pub const UNICODE_SETS: Self = Self(1 << 5);
    pub const UNICODE: Self = Self(1 << 6);
    pub const STICKY: Self = Self(1 << 7);

    pub fn has(self, flag: RegExpFlags) -> bool {
        self.0 & flag.0 == flag.0
    }

    fn insert(&mut self, flag: RegExpFlags) {
        self.0 |= flag.0;
    }

    pub fn bits(self) -> u8 {
        self.0
    }
}

/// JS_ENUMERATE_REGEXP_FLAGS: each flag with its flag character, in the order the C++ enumerates them.
pub const REGEXP_FLAGS_WITH_CHARACTERS: [(RegExpFlags, u8); 8] = [
    (RegExpFlags::HAS_INDICES, b'd'),
    (RegExpFlags::GLOBAL, b'g'),
    (RegExpFlags::IGNORE_CASE, b'i'),
    (RegExpFlags::MULTILINE, b'm'),
    (RegExpFlags::DOT_ALL, b's'),
    (RegExpFlags::UNICODE, b'u'),
    (RegExpFlags::UNICODE_SETS, b'v'),
    (RegExpFlags::STICKY, b'y'),
];

fn flag_for_code_unit(code_unit: u16) -> Option<RegExpFlags> {
    REGEXP_FLAGS_WITH_CHARACTERS
        .iter()
        .find(|(_, flag_character)| u16::from(*flag_character) == code_unit)
        .map(|(flag, _)| *flag)
}

/// The message of `error_type` with its one placeholder replaced by `code_unit`, as Utf16String::formatted() formats a
/// code unit of the flags into it.
fn message_with_code_unit(error_type: ErrorType, code_unit: u16) -> Utf16String {
    let (before, after) = error_type
        .format()
        .split_once("{}")
        .expect("the message has a placeholder for the flag");
    let mut builder = Utf16StringBuilder::new();
    builder.append_ascii(before);
    builder.append_code_unit(code_unit);
    builder.append_ascii(after);
    builder.to_utf16_string()
}

fn validate_flags(flags: Utf16View<'_>) -> Result<RegExpFlags, Utf16String> {
    let mut seen = [false; 128];
    let mut flag_bits = RegExpFlags::default();

    for code_unit in flags.code_units() {
        let Some(flag) = flag_for_code_unit(code_unit) else {
            return Err(message_with_code_unit(ErrorType::RegExpObjectBadFlag, code_unit));
        };
        if seen[code_unit as usize] {
            return Err(message_with_code_unit(ErrorType::RegExpObjectRepeatedFlag, code_unit));
        }
        seen[code_unit as usize] = true;
        flag_bits.insert(flag);
    }

    if flag_bits.has(RegExpFlags::UNICODE) && flag_bits.has(RegExpFlags::UNICODE_SETS) {
        return Err(Utf16String::from_utf8(&regexp_object_incompatible_flags('u', 'v')));
    }

    Ok(flag_bits)
}

fn to_flag_bits(flags: Utf16View<'_>) -> RegExpFlags {
    let mut flag_bits = RegExpFlags::default();
    for code_unit in flags.code_units() {
        if let Some(flag) = flag_for_code_unit(code_unit) {
            flag_bits.insert(flag);
        }
    }
    flag_bits
}

/// The flags regex::ECMAScriptRegex::compile() takes for a pattern with `flag_bits`.
pub fn compile_flags_for(flag_bits: RegExpFlags) -> EcmaScriptCompileFlags {
    EcmaScriptCompileFlags {
        global: flag_bits.has(RegExpFlags::GLOBAL),
        ignore_case: flag_bits.has(RegExpFlags::IGNORE_CASE),
        multiline: flag_bits.has(RegExpFlags::MULTILINE),
        dot_all: flag_bits.has(RegExpFlags::DOT_ALL),
        unicode: flag_bits.has(RegExpFlags::UNICODE),
        unicode_sets: flag_bits.has(RegExpFlags::UNICODE_SETS),
        sticky: flag_bits.has(RegExpFlags::STICKY),
        has_indices: flag_bits.has(RegExpFlags::HAS_INDICES),
    }
}

const LINE_SEPARATOR: u16 = 0x2028;
const PARAGRAPH_SEPARATOR: u16 = 0x2029;

#[repr(C)]
#[derive(Trace)]
pub struct RegExpObject {
    base: Object,
    #[gc(untraced)]
    pattern: GcRefCell<Utf16String>,
    #[gc(untraced)]
    flags: GcRefCell<Utf16String>,
    #[gc(untraced)]
    flag_bits: Cell<RegExpFlags>,
    #[gc(untraced)]
    legacy_features_enabled: Cell<bool>, // [[LegacyFeaturesEnabled]]
    /// The compiled pattern, which the C++ caches in a process-wide map shared by every RegExp object with the same
    /// pattern and flags, and which this object owns.
    #[gc(untraced)]
    cached_regex: GcRefCell<Option<Rc<EcmaScriptRegex>>>,
    // Note: This is initialized in RegExpAlloc, but will be non-null afterwards
    realm: Cell<Option<Gc<Realm>>>, // [[Realm]]
}

static REGEXP_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    initialize: RegExpObject::initialize,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(RegExpObject, Object, extends: [Object], methods: REGEXP_OBJECT_METHODS, finalize: finalize);

impl Deref for RegExpObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for RegExpObject {
    fn finalize(&self) {
        drop(self.cached_regex.replace(None));
    }
}

impl RegExpObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<RegExpObject> {
        realm.create_object(
            vm,
            Self::new(
                vm,
                Utf16String::default(),
                Utf16String::default(),
                realm.intrinsics().regexp_prototype(vm),
            ),
        )
    }

    pub fn create_with_pattern_and_flags(
        vm: &Vm,
        realm: Gc<Realm>,
        pattern: Utf16String,
        flags: Utf16String,
    ) -> Gc<RegExpObject> {
        realm.create_object(
            vm,
            Self::new(vm, pattern, flags, realm.intrinsics().regexp_prototype(vm)),
        )
    }

    fn new(vm: &Vm, pattern: Utf16String, flags: Utf16String, prototype: Gc<Object>) -> RegExpObject {
        let flag_bits = to_flag_bits(Utf16View::of_string(&flags));
        RegExpObject {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            pattern: GcRefCell::new(pattern),
            flags: GcRefCell::new(flags),
            flag_bits: Cell::new(flag_bits),
            legacy_features_enabled: Cell::new(false),
            cached_regex: GcRefCell::new(None),
            realm: Cell::new(None),
        }
    }

    fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
        object.define_direct_property(
            vm,
            &vm.names.lastIndex,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::WRITABLE),
        );
    }

    pub fn pattern(&self) -> Utf16String {
        self.pattern.borrow().clone()
    }

    pub fn flags(&self) -> Utf16String {
        self.flags.borrow().clone()
    }

    pub fn flag_bits(&self) -> RegExpFlags {
        self.flag_bits.get()
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm
            .get()
            .expect("RegExpAlloc and NewRegExp set the realm of every RegExp object")
    }

    pub fn legacy_features_enabled(&self) -> bool {
        self.legacy_features_enabled.get()
    }

    pub fn set_legacy_features_enabled(&self, legacy_features_enabled: bool) {
        self.legacy_features_enabled.set(legacy_features_enabled);
    }

    pub fn set_realm(&self, realm: Gc<Realm>) {
        self.realm.set(Some(realm));
    }

    pub fn cached_regex(&self) -> Option<Rc<EcmaScriptRegex>> {
        self.cached_regex.borrow().clone()
    }

    pub fn set_cached_regex(&self, regex: Option<Rc<EcmaScriptRegex>>) {
        drop(self.cached_regex.replace(regex));
    }

    // 22.2.3.3 RegExpInitialize ( obj, pattern, flags ), https://tc39.es/ecma262/#sec-regexpinitialize
    pub fn regexp_initialize(
        &self,
        vm: &Vm,
        pattern_value: Value,
        flags_value: Value,
    ) -> ThrowCompletionOr<Gc<RegExpObject>> {
        // Invalidate the cached compiled regex since the pattern/flags may change.
        self.set_cached_regex(None);

        // 1. If pattern is undefined, let P be the empty String.
        // 2. Else, let P be ? ToString(pattern).
        let pattern = if pattern_value.is_undefined() {
            Utf16String::default()
        } else {
            pattern_value.to_utf16_string(vm)?
        };

        // 3. If flags is undefined, let F be the empty String.
        // 4. Else, let F be ? ToString(flags).
        let flags = if flags_value.is_undefined() {
            Utf16String::default()
        } else {
            flags_value.to_utf16_string(vm)?
        };

        // 5. If F contains any code unit other than "d", "g", "i", "m", "s", "u", "v", or "y", or if F contains any code unit more than once, throw a SyntaxError exception.
        // 6. If F contains "i", let i be true; else let i be false.
        // 7. If F contains "m", let m be true; else let m be false.
        // 8. If F contains "s", let s be true; else let s be false.
        // 9. If F contains "u", let u be true; else let u be false.
        // 10. If F contains "v", let v be true; else let v be false.
        let flag_bits = match validate_flags(Utf16View::of_string(&flags)) {
            Ok(flag_bits) => flag_bits,
            Err(message) => return vm.throw_completion_with_utf16_message(ErrorKind::SyntaxError, message),
        };
        let unicode = flag_bits.has(RegExpFlags::UNICODE);
        let unicode_sets = flag_bits.has(RegExpFlags::UNICODE_SETS);

        let mut parsed_pattern = Vec::new();

        // Normalize non-ASCII code units to ASCII escapes before compiling the pattern.
        if !Utf16View::of_string(&pattern).is_empty() {
            let mut pattern_code_units = Vec::new();
            Utf16View::of_string(&pattern).append_to(&mut pattern_code_units);
            match parse_regex_pattern(&pattern_code_units, unicode, unicode_sets) {
                Ok(result) => parsed_pattern = result,
                Err(error) => {
                    return vm.throw_completion(ErrorKind::SyntaxError, ErrorType::RegExpCompileError, &[&error.error]);
                }
            }
        }

        // 11. If u is true and v is true, throw a SyntaxError exception.
        // NB: Already handled by validate_flags above.

        // Validate by trial-compiling the pattern.
        let mut compile_flags = compile_flags_for(flag_bits);
        compile_flags.has_indices = false;

        let compiled = match EcmaScriptRegex::compile(Utf16View::Utf16(&parsed_pattern), compile_flags) {
            Ok(compiled) => compiled,
            Err(error) => {
                return vm.throw_completion(ErrorKind::SyntaxError, ErrorType::RegExpCompileError, &[&error]);
            }
        };

        // Pattern and flag coercion can reenter and populate this cache.
        // NB: The trial compile is what exec would compile when the pattern does not have indices, so it is kept for
        //     exec instead of being compiled again.
        self.set_cached_regex(if flag_bits.has(RegExpFlags::HAS_INDICES) {
            None
        } else {
            Some(Rc::new(compiled))
        });

        // 16. Set obj.[[OriginalSource]] to P.
        drop(self.pattern.replace(pattern));

        // 17. Set obj.[[OriginalFlags]] to F.
        self.flag_bits.set(to_flag_bits(Utf16View::of_string(&flags)));
        drop(self.flags.replace(flags));

        // 18. Let capturingGroupsCount be CountLeftCapturingParensWithin(parseResult).
        // 19. Let rer be the RegExp Record { [[IgnoreCase]]: i, [[Multiline]]: m, [[DotAll]]: s, [[Unicode]]: u, [[CapturingGroupsCount]]: capturingGroupsCount }.
        // 20. Set obj.[[RegExpRecord]] to rer.
        // 21. Set obj.[[RegExpMatcher]] to CompilePattern of parseResult with argument rer.

        // 22. Perform ? Set(obj, "lastIndex", +0𝔽, true).
        self.base
            .set(vm, &vm.names.lastIndex, Value::from_i32(0), ShouldThrowExceptions::Yes)?;

        // 23. Return obj.
        Ok(self
            .as_gc()
            .downcast::<RegExpObject>()
            .expect("a RegExp object is a RegExpObject"))
    }

    // 22.2.6.13.1 EscapeRegExpPattern ( P, F ), https://tc39.es/ecma262/#sec-escaperegexppattern
    pub fn escape_regexp_pattern(&self) -> Utf16String {
        // 1. Let S be a String in the form of a Pattern[~UnicodeMode] (Pattern[+UnicodeMode] if F contains "u") equivalent
        //    to P interpreted as UTF-16 encoded Unicode code points (6.1.4), in which certain code points are escaped as
        //    described below. S may or may not be identical to P; however, the Abstract Closure that would result from
        //    evaluating S as a Pattern[~UnicodeMode] (Pattern[+UnicodeMode] if F contains "u") must behave identically to
        //    the Abstract Closure given by the constructed object's [[RegExpMatcher]] internal slot. Multiple calls to
        //    this abstract operation using the same values for P and F must produce identical results.
        // 2. The code points / or any LineTerminator occurring in the pattern shall be escaped in S as necessary to ensure
        //    that the string-concatenation of "/", S, "/", and F can be parsed (in an appropriate lexical context) as a
        //    RegularExpressionLiteral that behaves identically to the constructed regular expression. For example, if P is
        //    "/", then S could be "\/" or "/", among other possibilities, but not "/", because /// followed by F
        //    would be parsed as a SingleLineComment rather than a RegularExpressionLiteral. If P is the empty String, this
        //    specification can be met by letting S be "(?:)".
        // 3. Return S.
        let pattern = self.pattern();
        let pattern = Utf16View::of_string(&pattern);
        if pattern.is_empty() {
            return Utf16String::from_utf8("(?:)");
        }

        // FIXME: Check the 'u' and 'v' flags and escape accordingly
        // NB: The C++ walks the pattern by code point and appends each one back. Only code points of the BMP are ever
        //     escaped, so walking it by code unit gives the same string.
        let mut builder = Utf16StringBuilder::new();
        let mut escaped = false;
        let mut in_character_class = false;

        for code_unit in pattern.code_units() {
            if escaped {
                escaped = false;
                builder.append_ascii("\\");

                match code_unit {
                    0x0a => builder.append_ascii("n"),
                    0x0d => builder.append_ascii("r"),
                    LINE_SEPARATOR => builder.append_ascii("u2028"),
                    PARAGRAPH_SEPARATOR => builder.append_ascii("u2029"),
                    _ => builder.append_code_unit(code_unit),
                }
                continue;
            }

            if code_unit == u16::from(b'\\') {
                escaped = true;
                continue;
            }

            if code_unit == u16::from(b'[') {
                in_character_class = true;
            } else if code_unit == u16::from(b']') {
                in_character_class = false;
            }

            match code_unit {
                0x2f => {
                    if in_character_class {
                        builder.append_ascii("/");
                    } else {
                        builder.append_ascii("\\/");
                    }
                }
                0x0a => builder.append_ascii("\\n"),
                0x0d => builder.append_ascii("\\r"),
                LINE_SEPARATOR => builder.append_ascii("\\u2028"),
                PARAGRAPH_SEPARATOR => builder.append_ascii("\\u2029"),
                _ => builder.append_code_unit(code_unit),
            }
        }

        builder.to_utf16_string()
    }
}

// 22.2.3.1 RegExpCreate ( P, F ), https://tc39.es/ecma262/#sec-regexpcreate
pub fn regexp_create(vm: &Vm, pattern: Value, flags: Value) -> ThrowCompletionOr<Gc<RegExpObject>> {
    let realm = vm.current_realm().expect("RegExpCreate runs in a realm");

    // 1. Let obj be ! RegExpAlloc(%RegExp%).
    let regexp_object = regexp_alloc(vm, realm.intrinsics().regexp_constructor(vm).upcast()).must();

    // 2. Return ? RegExpInitialize(obj, P, F).
    regexp_object.regexp_initialize(vm, pattern, flags)
}

// 22.2.3.2 RegExpAlloc ( newTarget ), https://tc39.es/ecma262/#sec-regexpalloc
// 22.2.3.2 RegExpAlloc ( newTarget ), https://github.com/tc39/proposal-regexp-legacy-features#regexpalloc--newtarget-
pub fn regexp_alloc(vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<RegExpObject>> {
    let current_realm = vm.current_realm().expect("RegExpAlloc runs in a realm");

    // 1. Let obj be ? OrdinaryCreateFromConstructor(newTarget, "%RegExp.prototype%", « [[OriginalSource]], [[OriginalFlags]], [[RegExpRecord]], [[RegExpMatcher]] »).
    let regexp_object = ordinary_create_from_constructor_of(
        vm,
        current_realm,
        new_target,
        Intrinsics::regexp_prototype,
        |prototype| RegExpObject::new(vm, Utf16String::default(), Utf16String::default(), prototype),
    )?;

    // 2. Let thisRealm be the current Realm Record.
    let this_realm = vm.current_realm().expect("RegExpAlloc runs in a realm");

    // 3. Set the value of obj’s [[Realm]] internal slot to thisRealm.
    regexp_object.set_realm(this_realm);

    // 4. If SameValue(newTarget, thisRealm.[[Intrinsics]].[[%RegExp%]]) is true, then
    if same_value(
        Value::from_object(new_target),
        Value::from_object(this_realm.intrinsics().regexp_constructor(vm)),
    ) {
        // i. Set the value of obj’s [[LegacyFeaturesEnabled]] internal slot to true.
        regexp_object.set_legacy_features_enabled(true);
    }
    // 5. Else,
    else {
        // i. Set the value of obj’s [[LegacyFeaturesEnabled]] internal slot to false.
        regexp_object.set_legacy_features_enabled(false);
    }

    // 6. Perform ! DefinePropertyOrThrow(obj, "lastIndex", PropertyDescriptor { [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
    let mut descriptor = PropertyDescriptor {
        writable: Some(true),
        enumerable: Some(false),
        configurable: Some(false),
        ..Default::default()
    };
    regexp_object
        .define_property_or_throw(vm, &vm.names.lastIndex, &mut descriptor)
        .must();

    // 7. Return obj.
    Ok(regexp_object)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParseRegexPatternError {
    pub error: String,
}

// 22.2.3.4 Static Semantics: ParsePattern ( patternText, u, v ), https://tc39.es/ecma262/#sec-parsepattern
pub fn parse_regex_pattern(
    pattern: &[u16],
    unicode: bool,
    unicode_sets: bool,
) -> Result<Vec<u16>, ParseRegexPatternError> {
    if unicode && unicode_sets {
        return Err(ParseRegexPatternError {
            error: regexp_object_incompatible_flags('u', 'v'),
        });
    }

    let unicode_mode = unicode || unicode_sets;
    validate_named_group_name_surrogates(pattern, unicode_mode)?;

    let mut builder = Vec::with_capacity(pattern.len());
    let mut previous_code_unit_was_backslash = false;
    let mut index = 0;
    while index < pattern.len() {
        let code_unit = pattern[index];
        index += 1;

        if code_unit > 0x7f {
            // Incorrectly escaping this code unit will result in a wildly different regex than intended
            // as we're converting <c> to <\uhhhh>, which would turn into <\\uhhhh> if (incorrectly) escaped again,
            // leading to a matcher for the literal string "\uhhhh" instead of the intended code unit <c>.
            // As such, we're going to remove the (invalid) backslash and pretend it never existed.
            if !previous_code_unit_was_backslash {
                builder.push(u16::from(b'\\'));
            }
            previous_code_unit_was_backslash = false;

            if unicode_mode
                && is_utf16_high_surrogate(code_unit)
                && let Some(&next_code_unit) = pattern.get(index)
                && is_utf16_low_surrogate(next_code_unit)
            {
                let combined = decode_utf16_surrogate_pair(code_unit, next_code_unit);
                builder.extend(format!("u{{{combined:x}}}").encode_utf16());
                index += 1;
                continue;
            }

            if unicode_mode {
                builder.extend(format!("u{{{code_unit:04x}}}").encode_utf16());
            } else {
                builder.extend(format!("u{code_unit:04x}").encode_utf16());
            }
            continue;
        }

        builder.push(code_unit);
        previous_code_unit_was_backslash = code_unit == u16::from(b'\\') && !previous_code_unit_was_backslash;
    }

    Ok(builder)
}

/// ErrorType::RegExpObjectIncompatibleFlags.
fn regexp_object_incompatible_flags(first_flag: char, second_flag: char) -> String {
    format!("RegExp flag '{first_flag}' is incompatible with flag '{second_flag}'")
}

const HIGH_SURROGATE_MIN: u16 = 0xd800;
const HIGH_SURROGATE_MAX: u16 = 0xdbff;
const LOW_SURROGATE_MIN: u16 = 0xdc00;
const LOW_SURROGATE_MAX: u16 = 0xdfff;

fn is_utf16_high_surrogate(code_unit: u16) -> bool {
    (HIGH_SURROGATE_MIN..=HIGH_SURROGATE_MAX).contains(&code_unit)
}

fn is_utf16_low_surrogate(code_unit: u16) -> bool {
    (LOW_SURROGATE_MIN..=LOW_SURROGATE_MAX).contains(&code_unit)
}

fn decode_utf16_surrogate_pair(high_surrogate: u16, low_surrogate: u16) -> u32 {
    ((u32::from(high_surrogate - HIGH_SURROGATE_MIN)) << 10) + u32::from(low_surrogate - LOW_SURROGATE_MIN) + 0x10000
}

fn equals_ascii(code_unit: u16, ascii: u8) -> bool {
    code_unit == u16::from(ascii)
}

fn ascii_hex_digit_value(code_unit: u16) -> Option<u32> {
    char::from_u32(u32::from(code_unit)).and_then(|character| character.to_digit(16))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RegExpNameElementKind {
    CodePoint,
    HighSurrogate,
    LowSurrogate,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RegExpNameElementOrigin {
    Literal,
    FixedEscape,
    BracedEscape,
}

struct RegExpNameElement {
    kind: RegExpNameElementKind,
    origin: RegExpNameElementOrigin,
    next_index: usize,
}

fn invalid_group_name_error() -> ParseRegexPatternError {
    ParseRegexPatternError {
        error: "invalid group name".to_owned(),
    }
}

/// The C++ classifies escaped values through the u16 surrogate predicates, so a braced escape above the BMP is
/// classified by its low 16 bits (\u{1DF00} counts as a low surrogate). That narrowing is kept so that the same group
/// names are rejected.
fn classify_escaped_value(value: u32) -> RegExpNameElementKind {
    let truncated_to_code_unit = value as u16;
    if is_utf16_high_surrogate(truncated_to_code_unit) {
        RegExpNameElementKind::HighSurrogate
    } else if is_utf16_low_surrogate(truncated_to_code_unit) {
        RegExpNameElementKind::LowSurrogate
    } else {
        RegExpNameElementKind::CodePoint
    }
}

fn parse_regexp_name_element(pattern: &[u16], index: usize) -> Result<RegExpNameElement, ParseRegexPatternError> {
    let length = pattern.len();
    let Some(&code_unit) = pattern.get(index) else {
        return Err(invalid_group_name_error());
    };

    if !equals_ascii(code_unit, b'\\') {
        let (kind, next_index) = if is_utf16_high_surrogate(code_unit) {
            match pattern.get(index + 1) {
                Some(&next_code_unit) if is_utf16_low_surrogate(next_code_unit) => {
                    (RegExpNameElementKind::CodePoint, index + 2)
                }
                _ => (RegExpNameElementKind::HighSurrogate, index + 1),
            }
        } else if is_utf16_low_surrogate(code_unit) {
            (RegExpNameElementKind::LowSurrogate, index + 1)
        } else {
            (RegExpNameElementKind::CodePoint, index + 1)
        };
        return Ok(RegExpNameElement {
            kind,
            origin: RegExpNameElementOrigin::Literal,
            next_index,
        });
    }

    if !pattern.get(index + 1).is_some_and(|&next| equals_ascii(next, b'u')) {
        return Err(invalid_group_name_error());
    }

    let mut escape_index = index + 2;
    if pattern.get(escape_index).is_some_and(|&next| equals_ascii(next, b'{')) {
        escape_index += 1;

        let mut value = 0u32;
        let mut digits = 0usize;
        while escape_index < length && !equals_ascii(pattern[escape_index], b'}') {
            let Some(digit) = ascii_hex_digit_value(pattern[escape_index]) else {
                return Err(invalid_group_name_error());
            };
            value = value * 16 + digit;
            if value > 0x10ffff {
                return Err(invalid_group_name_error());
            }
            digits += 1;
            escape_index += 1;
        }

        if digits == 0 || escape_index >= length {
            return Err(invalid_group_name_error());
        }

        return Ok(RegExpNameElement {
            kind: classify_escaped_value(value),
            origin: RegExpNameElementOrigin::BracedEscape,
            next_index: escape_index + 1,
        });
    }

    let Some(hex_digits) = pattern.get(escape_index..escape_index + 4) else {
        return Err(invalid_group_name_error());
    };
    let mut value = 0u32;
    for &digit in hex_digits {
        let Some(digit) = ascii_hex_digit_value(digit) else {
            return Err(invalid_group_name_error());
        };
        value = value * 16 + digit;
    }

    Ok(RegExpNameElement {
        kind: classify_escaped_value(value),
        origin: RegExpNameElementOrigin::FixedEscape,
        next_index: escape_index + 4,
    })
}

/// Checks the group name starting at name_start, and returns the index just past its closing '>'.
fn validate_regexp_name_surrogates(pattern: &[u16], name_start: usize) -> Result<usize, ParseRegexPatternError> {
    let mut index = name_start;

    while index < pattern.len() {
        if equals_ascii(pattern[index], b'>') {
            return Ok(index + 1);
        }

        let element = parse_regexp_name_element(pattern, index)?;
        if element.kind == RegExpNameElementKind::CodePoint {
            index = element.next_index;
            continue;
        }

        if element.kind == RegExpNameElementKind::LowSurrogate {
            return Err(invalid_group_name_error());
        }

        let next_element = parse_regexp_name_element(pattern, element.next_index)?;
        if next_element.kind != RegExpNameElementKind::LowSurrogate
            || element.origin != next_element.origin
            || element.origin == RegExpNameElementOrigin::BracedEscape
        {
            return Err(invalid_group_name_error());
        }

        index = next_element.next_index;
    }

    Err(invalid_group_name_error())
}

fn starts_named_group_or_lookbehind(pattern: &[u16], index: usize) -> bool {
    equals_ascii(pattern[index], b'(')
        && pattern.get(index + 1).is_some_and(|&next| equals_ascii(next, b'?'))
        && pattern.get(index + 2).is_some_and(|&next| equals_ascii(next, b'<'))
}

fn starts_lookbehind(pattern: &[u16], index: usize) -> bool {
    starts_named_group_or_lookbehind(pattern, index)
        && pattern
            .get(index + 3)
            .is_some_and(|&next| equals_ascii(next, b'=') || equals_ascii(next, b'!'))
}

fn pattern_has_named_capture_groups(pattern: &[u16]) -> bool {
    let mut in_character_class = false;
    let mut index = 0;

    while index < pattern.len() {
        let code_unit = pattern[index];

        if equals_ascii(code_unit, b'\\') {
            index += 2;
            continue;
        }

        if equals_ascii(code_unit, b'[') && !in_character_class {
            in_character_class = true;
        } else if equals_ascii(code_unit, b']') && in_character_class {
            in_character_class = false;
        } else if !in_character_class
            && starts_named_group_or_lookbehind(pattern, index)
            && !starts_lookbehind(pattern, index)
        {
            return true;
        }

        index += 1;
    }

    false
}

fn validate_named_group_name_surrogates(pattern: &[u16], unicode_aware: bool) -> Result<(), ParseRegexPatternError> {
    let mut in_character_class = false;
    let has_named_groups_or_unicode = unicode_aware || pattern_has_named_capture_groups(pattern);
    let mut index = 0;

    while index < pattern.len() {
        let code_unit = pattern[index];

        if equals_ascii(code_unit, b'\\') {
            if has_named_groups_or_unicode
                && !in_character_class
                && pattern.get(index + 1).is_some_and(|&next| equals_ascii(next, b'k'))
                && pattern.get(index + 2).is_some_and(|&next| equals_ascii(next, b'<'))
            {
                index = validate_regexp_name_surrogates(pattern, index + 3)?;
                continue;
            }

            index += 2;
            continue;
        }

        if equals_ascii(code_unit, b'[') && !in_character_class {
            in_character_class = true;
        } else if equals_ascii(code_unit, b']') && in_character_class {
            in_character_class = false;
        } else if !in_character_class
            && starts_named_group_or_lookbehind(pattern, index)
            && index + 3 < pattern.len()
            && !starts_lookbehind(pattern, index)
        {
            index = validate_regexp_name_surrogates(pattern, index + 3)?;
            continue;
        }

        index += 1;
    }

    Ok(())
}
