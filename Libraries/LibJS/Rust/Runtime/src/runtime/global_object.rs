/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt::Write;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, define_cell};
use crate::interpreter::vm::{EvalMode, Vm};
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{CallerMode, perform_eval};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::object::{IntrinsicAccessor, MayInterfereWithIndexedPropertyAccess};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::string_conversions::parse_first_number_f64;
use crate::runtime::value_conversions::is_js_whitespace;
use crate::utf16::Utf16View;

/// The global object of a realm, which SetDefaultGlobalBindings gives the properties of clause 19. Hosts that need
/// more extend it.
#[repr(C)]
#[derive(Trace)]
pub struct GlobalObject {
    base: Object,
}

define_cell!(GlobalObject, Object, extends: [Object]);

impl Deref for GlobalObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl GlobalObject {
    /// GlobalObject(Realm&), for `class`, which is GlobalObject or a class that extends it.
    pub fn new(vm: &Vm, class: &'static Class, realm: Gc<Realm>) -> GlobalObject {
        let base = Object::new_global_object(vm, class, realm, MayInterfereWithIndexedPropertyAccess::No);
        base.set_prototype(vm, Some(realm.object_prototype()));
        GlobalObject { base }
    }
}

impl Object {
    pub fn is_global_object(&self) -> bool {
        self.is::<GlobalObject>()
    }
}

// 9.3.3 SetDefaultGlobalBindings ( realmRec ), https://tc39.es/ecma262/#sec-setdefaultglobalbindings
pub fn set_default_global_bindings(vm: &Vm, realm: Gc<Realm>) {
    let names = &vm.names;

    // 1. Let global be realmRec.[[GlobalObject]].
    let global = realm.global_object();

    // 2. For each property of the Global Object specified in clause 19, do
    //     a. Let name be the String value of the property name.
    //     b. Let desc be the fully populated data Property Descriptor for the property, containing the specified attributes for the property.
    //        For properties listed in 19.2, 19.3, or 19.4 the value of the [[Value]] attribute is the corresponding intrinsic object from realmRec.
    //     c. Perform ? DefinePropertyOrThrow(global, name, desc).
    //     NOTE: This function is infallible as we set properties directly; property clashes in global object construction are not expected.

    let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
    let none = PropertyAttributes::new(0);
    let define_intrinsic_accessor = |name: &PropertyKey, accessor: IntrinsicAccessor| {
        global.define_intrinsic_accessor(vm, name, attr, accessor);
    };

    // 19.2 Function Properties of the Global Object, https://tc39.es/ecma262/#sec-function-properties-of-the-global-object
    let define_intrinsic_function = |name: &PropertyKey, function: Gc<FunctionObject>| {
        global.define_direct_property(vm, name, Value::from_object(function), attr);
    };
    let intrinsics = realm.intrinsics();
    define_intrinsic_function(&names.eval, intrinsics.eval_function());
    define_intrinsic_function(&names.isFinite, intrinsics.is_finite_function());
    define_intrinsic_function(&names.isNaN, intrinsics.is_nan_function());
    define_intrinsic_function(&names.parseFloat, intrinsics.parse_float_function());
    define_intrinsic_function(&names.parseInt, intrinsics.parse_int_function());
    define_intrinsic_function(&names.decodeURI, intrinsics.decode_uri_function());
    define_intrinsic_function(&names.decodeURIComponent, intrinsics.decode_uri_component_function());
    define_intrinsic_function(&names.encodeURI, intrinsics.encode_uri_function());
    define_intrinsic_function(&names.encodeURIComponent, intrinsics.encode_uri_component_function());

    // 19.1 Value Properties of the Global Object, https://tc39.es/ecma262/#sec-value-properties-of-the-global-object
    global.define_direct_property(
        vm,
        &names.globalThis,
        Value::from_object(realm.global_environment().global_this_value()),
        attr,
    );
    global.define_direct_property(vm, &names.Infinity, Value::from_f64(f64::INFINITY), none);
    global.define_direct_property(vm, &names.NaN, Value::from_f64(f64::NAN), none);
    global.define_direct_property(vm, &names.undefined, Value::UNDEFINED, none);

    // 19.3 Constructor Properties of the Global Object, https://tc39.es/ecma262/#sec-constructor-properties-of-the-global-object
    // NB: The constructors of the builtins the runtime does not have yet are left out, keeping the order of the others.
    define_intrinsic_accessor(&names.AggregateError, |vm, realm| {
        Value::from_object(realm.intrinsics().aggregate_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.Array, |vm, realm| {
        Value::from_object(realm.intrinsics().array_constructor(vm))
    });
    define_intrinsic_accessor(&names.BigInt, |vm, realm| {
        Value::from_object(realm.intrinsics().bigint_constructor(vm))
    });
    define_intrinsic_accessor(&names.Boolean, |vm, realm| {
        Value::from_object(realm.intrinsics().boolean_constructor(vm))
    });
    define_intrinsic_accessor(&names.Error, |vm, realm| {
        Value::from_object(realm.intrinsics().error_constructor(vm))
    });
    define_intrinsic_accessor(&names.EvalError, |vm, realm| {
        Value::from_object(realm.intrinsics().eval_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.FinalizationRegistry, |vm, realm| {
        Value::from_object(realm.intrinsics().finalization_registry_constructor(vm))
    });
    define_intrinsic_accessor(&names.Function, |vm, realm| {
        Value::from_object(realm.intrinsics().function_constructor(vm))
    });
    define_intrinsic_accessor(&names.Iterator, |vm, realm| {
        Value::from_object(realm.intrinsics().iterator_constructor(vm))
    });
    define_intrinsic_accessor(&names.Map, |vm, realm| {
        Value::from_object(realm.intrinsics().map_constructor(vm))
    });
    define_intrinsic_accessor(&names.Number, |vm, realm| {
        Value::from_object(realm.intrinsics().number_constructor(vm))
    });
    define_intrinsic_accessor(&names.Object, |vm, realm| {
        Value::from_object(realm.intrinsics().object_constructor(vm))
    });
    define_intrinsic_accessor(&names.Promise, |vm, realm| {
        Value::from_object(realm.intrinsics().promise_constructor(vm))
    });
    define_intrinsic_accessor(&names.Proxy, |_, realm| {
        Value::from_object(realm.intrinsics().proxy_constructor())
    });
    define_intrinsic_accessor(&names.RangeError, |vm, realm| {
        Value::from_object(realm.intrinsics().range_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.ReferenceError, |vm, realm| {
        Value::from_object(realm.intrinsics().reference_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.RegExp, |vm, realm| {
        Value::from_object(realm.intrinsics().regexp_constructor(vm))
    });
    define_intrinsic_accessor(&names.Set, |vm, realm| {
        Value::from_object(realm.intrinsics().set_constructor(vm))
    });
    define_intrinsic_accessor(&names.String, |vm, realm| {
        Value::from_object(realm.intrinsics().string_constructor(vm))
    });
    define_intrinsic_accessor(&names.Symbol, |vm, realm| {
        Value::from_object(realm.intrinsics().symbol_constructor(vm))
    });
    define_intrinsic_accessor(&names.SyntaxError, |vm, realm| {
        Value::from_object(realm.intrinsics().syntax_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.TypeError, |vm, realm| {
        Value::from_object(realm.intrinsics().type_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.URIError, |vm, realm| {
        Value::from_object(realm.intrinsics().uri_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.WeakMap, |vm, realm| {
        Value::from_object(realm.intrinsics().weak_map_constructor(vm))
    });
    define_intrinsic_accessor(&names.WeakRef, |vm, realm| {
        Value::from_object(realm.intrinsics().weak_ref_constructor(vm))
    });
    define_intrinsic_accessor(&names.WeakSet, |vm, realm| {
        Value::from_object(realm.intrinsics().weak_set_constructor(vm))
    });

    // 19.4 Other Properties of the Global Object, https://tc39.es/ecma262/#sec-other-properties-of-the-global-object
    // NB: Atomics and Intl come with their builtins, before JSON.
    define_intrinsic_accessor(&names.JSON, |vm, realm| {
        Value::from_object(realm.intrinsics().json_object(vm))
    });
    define_intrinsic_accessor(&names.Math, |vm, realm| {
        Value::from_object(realm.intrinsics().math_object(vm))
    });
    define_intrinsic_accessor(&names.Reflect, |vm, realm| {
        Value::from_object(realm.intrinsics().reflect_object(vm))
    });
    // NB: Temporal comes with its builtins, after Reflect.

    // B.2.1 Additional Properties of the Global Object, https://tc39.es/ecma262/#sec-additional-properties-of-the-global-object
    define_intrinsic_function(&names.escape, intrinsics.escape_function());
    define_intrinsic_function(&names.unescape, intrinsics.unescape_function());

    // Non-standard
    global.define_direct_property(
        vm,
        &names.InternalError,
        Value::from_object(realm.intrinsics().internal_error_constructor(vm)),
        attr,
    );
    global.define_direct_property(
        vm,
        &names.console,
        Value::from_object(realm.intrinsics().console_object(vm)),
        attr,
    );

    // 3. Return unused.
}

fn js_nan() -> Value {
    Value::from_f64(f64::NAN)
}

/// TrimString(string, start) of StringPrototype.cpp, for a string that is already a String: the string without its
/// leading white space.
// FIXME: Use the TrimString of the String builtins once they exist.
fn trim_string_start(string: Utf16View<'_>) -> Utf16View<'_> {
    let length = string.length_in_code_units();
    let start = (0..length)
        .find(|index| !is_js_whitespace(string.code_unit_at(*index)))
        .unwrap_or(length);
    string.substring_view(start, length - start)
}

/// The Record CodePointAt ( string, position ) returns.
struct CodePoint {
    is_unpaired_surrogate: bool,
    code_point: u32,
    code_unit_count: usize,
}

// 11.1.4 CodePointAt ( string, position ), https://tc39.es/ecma262/#sec-codepointat
// FIXME: Use the CodePointAt of the String builtins once they exist.
fn code_point_at(string: Utf16View<'_>, position: usize) -> CodePoint {
    // 1. Let size be the length of string.
    let size = string.length_in_code_units();

    // 2. Assert: position ≥ 0 and position < size.
    assert!(position < size);

    // 3. Let first be the code unit at index position within string.
    let first = string.code_unit_at(position);

    // 4. Let cp be the code point whose numeric value is the numeric value of first.
    let code_point = u32::from(first);

    // 5. If first is neither a leading surrogate nor a trailing surrogate, then
    if !(0xD800..=0xDFFF).contains(&first) {
        // a. Return the Record { [[CodePoint]]: cp, [[CodeUnitCount]]: 1, [[IsUnpairedSurrogate]]: false }.
        return CodePoint {
            is_unpaired_surrogate: false,
            code_point,
            code_unit_count: 1,
        };
    }

    // 6. If first is a trailing surrogate or position + 1 = size, then
    if (0xDC00..=0xDFFF).contains(&first) || position + 1 == size {
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
    if !(0xDC00..=0xDFFF).contains(&second) {
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
        code_point: 0x10000 + ((u32::from(first) - 0xD800) << 10) + (u32::from(second) - 0xDC00),
        code_unit_count: 2,
    }
}

fn is_ascii_alphanumeric(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|byte| byte.is_ascii_alphanumeric())
}

fn is_ascii_hex_digit(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|byte| byte.is_ascii_hexdigit())
}

fn parse_ascii_hex_digit(code_unit: u16) -> u16 {
    char::from_u32(u32::from(code_unit))
        .and_then(|character| character.to_digit(16))
        .expect("the code unit is a hex digit") as u16
}

/// What appendff("{:02X}") and appendff("{:04X}") append: the uppercase hexadecimal digits of `number`, zero-padded to
/// `width`.
fn append_uppercase_hex(builder: &mut Vec<u16>, number: u32, width: usize) {
    let mut digits = String::new();
    write!(digits, "{number:0width$X}").expect("formatting into a String cannot fail");
    builder.extend(digits.bytes().map(u16::from));
}

fn append_ascii(builder: &mut Vec<u16>, text: &str) {
    builder.extend(text.bytes().map(u16::from));
}

impl GlobalObject {
    // NB: 19.2.1 eval ( x ) comes with eval.

    // 19.2.2 isFinite ( number ), https://tc39.es/ecma262/#sec-isfinite-number
    pub(crate) fn is_finite(vm: &Vm) -> ThrowCompletionOr<Value> {
        let number = vm.argument(0);

        // 1. Let num be ? ToNumber(number).
        let num = number.to_number(vm)?;

        // 2. If num is not finite, return false.
        // 3. Otherwise, return true.
        Ok(Value::from_bool(num.is_finite_number()))
    }

    // 19.2.3 isNaN ( number ), https://tc39.es/ecma262/#sec-isnan-number
    pub(crate) fn is_nan(vm: &Vm) -> ThrowCompletionOr<Value> {
        let number = vm.argument(0);

        // 1. Let num be ? ToNumber(number).
        let num = number.to_number(vm)?;

        // 2. If num is NaN, return true.
        // 3. Otherwise, return false.
        Ok(Value::from_bool(num.is_nan()))
    }

    // 19.2.4 parseFloat ( string ), https://tc39.es/ecma262/#sec-parsefloat-string
    pub(crate) fn parse_float(vm: &Vm) -> ThrowCompletionOr<Value> {
        let string = vm.argument(0);

        // OPTIMIZATION: We can skip the number-to-string-to-number round trip when the value is already a number.
        if string.is_number() {
            // Special case for negative zero - it should become positive zero
            if string.is_negative_zero() {
                return Ok(Value::from_i32(0));
            }

            return Ok(string);
        }

        // 1. Let inputString be ? ToString(string).
        let input_string = string.to_utf16_string(vm)?;

        // 2. Let trimmedString be ! TrimString(inputString, start).
        let trimmed_string = trim_string_start(Utf16View::of_string(&input_string));
        if trimmed_string.is_empty() {
            return Ok(js_nan());
        }

        // 3. If neither trimmedString nor any prefix of trimmedString satisfies the syntax of a StrDecimalLiteral (see 7.1.4.1), return NaN.
        // 4. Let numberString be the longest prefix of trimmedString, which might be trimmedString itself, that satisfies the syntax of a StrDecimalLiteral.
        // 5. Let parsedNumber be ParseText(StringToCodePoints(numberString), StrDecimalLiteral).
        // 6. Assert: parsedNumber is a Parse Node.
        // 7. Return StringNumericValue of parsedNumber.
        Ok(trimmed_string.with_utf16_code_units(|trimmed_string_view| {
            if let Some(parsed_number) = parse_first_number_f64(trimmed_string_view) {
                return Value::from_f64(parsed_number.value);
            }

            let first_code_point = trimmed_string_view[0];
            let mut trimmed_string_view = trimmed_string_view;
            if first_code_point == u16::from(b'-') || first_code_point == u16::from(b'+') {
                trimmed_string_view = &trimmed_string_view[1..];
            }

            if Utf16View::Utf16(trimmed_string_view).length_in_code_units() >= 8
                && Utf16View::Utf16(&trimmed_string_view[..8]) == "Infinity"
            {
                // Only an immediate - means we should return negative infinity
                return Value::from_f64(if first_code_point == u16::from(b'-') {
                    f64::NEG_INFINITY
                } else {
                    f64::INFINITY
                });
            }

            js_nan()
        }))
    }

    // 19.2.5 parseInt ( string, radix ), https://tc39.es/ecma262/#sec-parseint-string-radix
    pub(crate) fn parse_int(vm: &Vm) -> ThrowCompletionOr<Value> {
        let string = vm.argument(0);

        // 1. Let inputString be ? ToString(string).
        let input_string = string.to_utf16_string(vm)?;
        let input_string = Utf16View::of_string(&input_string);

        // 2. Let S be ! TrimString(inputString, start).
        // OPTIMIZATION: We can skip the trimming step when the value already starts with an alphanumeric ASCII character.
        let trimmed_string = if input_string.is_empty() || is_ascii_alphanumeric(input_string.code_unit_at(0)) {
            input_string
        } else {
            trim_string_start(input_string)
        };

        // 3. Let sign be 1.
        let mut sign = 1;

        // 4. If S is not empty and the first code unit of S is the code unit 0x002D (HYPHEN-MINUS), set sign to -1.
        let first_code_point = (!trimmed_string.is_empty()).then(|| trimmed_string.code_unit_at(0));
        if first_code_point == Some(0x2D) {
            sign = -1;
        }

        // 5. If S is not empty and the first code unit of S is the code unit 0x002B (PLUS SIGN) or the code unit 0x002D (HYPHEN-MINUS), remove the first code unit from S.
        let mut trimmed_view = trimmed_string;
        if first_code_point == Some(0x2B) || first_code_point == Some(0x2D) {
            trimmed_view = trimmed_view.substring_view(1, trimmed_view.length_in_code_units() - 1);
        }

        // 6. Let R be ℝ(? ToInt32(radix)).
        let mut radix = vm.argument(1).to_i32(vm)?;

        // 7. Let stripPrefix be true.
        let mut strip_prefix = true;

        // 8. If R ≠ 0, then
        if radix != 0 {
            // a. If R < 2 or R > 36, return NaN.
            if !(2..=36).contains(&radix) {
                return Ok(js_nan());
            }

            // b. If R ≠ 16, set stripPrefix to false.
            if radix != 16 {
                strip_prefix = false;
            }
        }
        // 9. Else,
        else {
            // a. Set R to 10.
            radix = 10;
        }

        // 10. If stripPrefix is true, then
        if strip_prefix {
            // a. If the length of S is at least 2 and the first two code units of S are either "0x" or "0X", then
            if trimmed_view.length_in_code_units() >= 2
                && trimmed_view.code_unit_at(0) == u16::from(b'0')
                && (trimmed_view.code_unit_at(1) == u16::from(b'x') || trimmed_view.code_unit_at(1) == u16::from(b'X'))
            {
                // i. Remove the first two code units from S.
                trimmed_view = trimmed_view.substring_view(2, trimmed_view.length_in_code_units() - 2);

                // ii. Set R to 16.
                radix = 16;
            }
        }

        // 11. If S contains a code unit that is not a radix-R digit, let end be the index within S of the first such code unit; otherwise, let end be the length of S.
        // 12. Let Z be the substring of S from 0 to end.
        // 13. If Z is empty, return NaN.
        // 14. Let mathInt be the integer value that is represented by Z in radix-R notation, using the letters A-Z and a-z for digits with values 10 through 35. (However, if R is 10 and Z contains more than 20 significant digits, every significant digit after the 20th may be replaced by a 0 digit, at the option of the implementation; and if R is not 2, 4, 8, 10, 16, or 32, then mathInt may be an implementation-approximated integer representing the integer value denoted by Z in radix-R notation.)
        let parse_digit = |code_unit: u16| -> Option<u32> {
            if !is_ascii_alphanumeric(code_unit) {
                return None;
            }
            let digit = char::from(code_unit as u8)
                .to_digit(36)
                .expect("an ASCII alphanumeric character is a base 36 digit");
            if digit >= radix as u32 {
                return None;
            }
            Some(digit)
        };

        let mut had_digits = false;
        let mut number = 0.0;
        // NB: Only ASCII code units are digits, so going through the code units stops where going through the code
        //     points does.
        for code_unit in trimmed_view.code_units() {
            let Some(digit) = parse_digit(code_unit) else {
                break;
            };
            had_digits = true;
            number *= f64::from(radix);
            number += f64::from(digit);
        }

        if !had_digits {
            return Ok(js_nan());
        }

        // 15. If mathInt = 0, then
        // a. If sign = -1, return -0𝔽.
        // b. Return +0𝔽.
        // 16. Return 𝔽(sign × mathInt).
        Ok(Value::from_f64(f64::from(sign) * number))
    }

    // 19.2.6.1 decodeURI ( encodedURI ), https://tc39.es/ecma262/#sec-decodeuri-encodeduri
    pub(crate) fn decode_uri(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let uriString be ? ToString(encodedURI).
        let uri_string = vm.argument(0).to_utf16_string(vm)?;

        // 2. Let preserveEscapeSet be ";/?:@&=+$,#".
        // 3. Return ? Decode(uriString, preserveEscapeSet).
        let decoded = decode(vm, Utf16View::of_string(&uri_string), ";/?:@&=+$,#")?;
        Ok(Value::from_string(PrimitiveString::create(vm, decoded)))
    }

    // 19.2.6.2 decodeURIComponent ( encodedURIComponent ), https://tc39.es/ecma262/#sec-decodeuricomponent-encodeduricomponent
    pub(crate) fn decode_uri_component(vm: &Vm) -> ThrowCompletionOr<Value> {
        let encoded_uri_component = vm.argument(0);

        // 1. Let componentString be ? ToString(encodedURIComponent).
        let uri_string = encoded_uri_component.to_utf16_string(vm)?;

        // 2. Let preserveEscapeSet be the empty String.
        // 3. Return ? Decode(componentString, preserveEscapeSet).
        let decoded = decode(vm, Utf16View::of_string(&uri_string), "")?;
        Ok(Value::from_string(PrimitiveString::create(vm, decoded)))
    }

    // 19.2.6.3 encodeURI ( uri ), https://tc39.es/ecma262/#sec-encodeuri-uri
    pub(crate) fn encode_uri(vm: &Vm) -> ThrowCompletionOr<Value> {
        let uri = vm.argument(0);

        // 1. Let uriString be ? ToString(uri).
        let uri_string = uri.to_utf16_string(vm)?;

        // 2. Let extraUnescaped be ";/?:@&=+$,#".
        // 3. Return ? Encode(uriString, extraUnescaped).
        let encoded = encode(
            vm,
            Utf16View::of_string(&uri_string),
            ";/?:@&=+$,abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.!~*'()#",
        )?;
        Ok(Value::from_string(PrimitiveString::create(vm, encoded)))
    }

    // 19.2.6.4 encodeURIComponent ( uriComponent ), https://tc39.es/ecma262/#sec-encodeuricomponent-uricomponent
    pub(crate) fn encode_uri_component(vm: &Vm) -> ThrowCompletionOr<Value> {
        let uri_component = vm.argument(0);

        // 1. Let componentString be ? ToString(uriComponent).
        let uri_string = uri_component.to_utf16_string(vm)?;

        // 2. Let extraUnescaped be the empty String.
        // 3. Return ? Encode(componentString, extraUnescaped).
        let encoded = encode(
            vm,
            Utf16View::of_string(&uri_string),
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.!~*'()",
        )?;
        Ok(Value::from_string(PrimitiveString::create(vm, encoded)))
    }

    // B.2.1.1 escape ( string ), https://tc39.es/ecma262/#sec-escape-string
    pub(crate) fn escape(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set string to ? ToString(string).
        let string = vm.argument(0).to_utf16_string(vm)?;
        let string = Utf16View::of_string(&string);

        // 3. Let R be the empty String.
        let mut escaped = Vec::with_capacity(string.length_in_code_units());

        // 4. Let unescapedSet be the string-concatenation of the ASCII word characters and "@*+-./".
        let unescaped_set = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@*_+-./";

        // 2. Let length be the length of string.
        // 5. Let k be 0.
        // 6. Repeat, while k < length,
        for code_unit in string.code_units() {
            // a. Let char be the code unit at index k within string.

            // b. If unescapedSet contains char, then
            // NOTE: We know unescapedSet is ASCII-only, so ensure we have an ASCII codepoint before casting to char.
            if code_unit < 0x80 && unescaped_set.contains(&(code_unit as u8)) {
                // i. Let S be the String value containing the single code unit char.
                escaped.push(code_unit);
            }
            // c. Else,
            // i. Let n be the numeric value of char.
            // ii. If n < 256, then
            else if code_unit < 256 {
                // 1. Let hex be the String representation of n, formatted as an uppercase hexadecimal number.
                // 2. Let S be the string-concatenation of "%" and ! StringPad(hex, 2𝔽, "0", start).
                append_ascii(&mut escaped, "%");
                append_uppercase_hex(&mut escaped, u32::from(code_unit), 2);
            }
            // iii. Else,
            else {
                // 1. Let hex be the String representation of n, formatted as an uppercase hexadecimal number.
                // 2. Let S be the string-concatenation of "%u" and ! StringPad(hex, 4𝔽, "0", start).
                append_ascii(&mut escaped, "%u");
                append_uppercase_hex(&mut escaped, u32::from(code_unit), 4);
            }

            // d. Set R to the string-concatenation of R and S.
            // e. Set k to k + 1.
        }

        // 7. Return R.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            Utf16String::from_utf16(&escaped),
        )))
    }

    // B.2.1.2 unescape ( string ), https://tc39.es/ecma262/#sec-unescape-string
    pub(crate) fn unescape(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set string to ? ToString(string).
        let string = vm.argument(0).to_utf16_string(vm)?;
        let string = Utf16View::of_string(&string);

        // 2. Let length be the length of string.
        let length = string.length_in_code_units();

        // 3. Let R be the empty String.
        let mut unescaped = Vec::with_capacity(length);

        // 4. Let k be 0.
        let mut k = 0;

        // 5. Repeat, while k ≠ length,
        while k < length {
            // a. Let c be the code unit at index k within string.
            let mut code_unit = string.code_unit_at(k);

            // b. If c is the code unit 0x0025 (PERCENT SIGN), then
            if code_unit == u16::from(b'%') {
                // i. Let hexEscape be the empty String.
                // ii. Let skip be 0.
                // iii. If k ≤ length - 6 and the code unit at index k + 1 within string is the code unit 0x0075 (LATIN SMALL LETTER U), then
                if k + 5 < length
                    && string.code_unit_at(k + 1) == u16::from(b'u')
                    && (k + 2..k + 6).all(|index| is_ascii_hex_digit(string.code_unit_at(index)))
                {
                    // 1. Set hexEscape to the substring of string from k + 2 to k + 6.
                    code_unit = (parse_ascii_hex_digit(string.code_unit_at(k + 2)) << 12)
                        | (parse_ascii_hex_digit(string.code_unit_at(k + 3)) << 8)
                        | (parse_ascii_hex_digit(string.code_unit_at(k + 4)) << 4)
                        | parse_ascii_hex_digit(string.code_unit_at(k + 5));

                    // 2. Set skip to 5.
                    k += 5;
                }
                // iv. Else if k ≤ length - 3, then
                else if k + 2 < length
                    && is_ascii_hex_digit(string.code_unit_at(k + 1))
                    && is_ascii_hex_digit(string.code_unit_at(k + 2))
                {
                    // 1. Set hexEscape to the substring of string from k + 1 to k + 3.
                    code_unit = (parse_ascii_hex_digit(string.code_unit_at(k + 1)) << 4)
                        | parse_ascii_hex_digit(string.code_unit_at(k + 2));

                    // 2. Set skip to 2.
                    k += 2;
                }

                // v. If hexEscape can be interpreted as an expansion of HexDigits[~Sep], then
                //    1. Let hexIntegerLiteral be the string-concatenation of "0x" and hexEscape.
                //    2. Let n be ! ToNumber(hexIntegerLiteral).
                //    3. Set c to the code unit whose value is ℝ(n).
                //    4. Set k to k + skip.
                // NOTE: All of this is already done in the branches above.
            }

            // c. Set R to the string-concatenation of R and c.
            unescaped.push(code_unit);

            // d. Set k to k + 1.
            k += 1;
        }

        // 6. Return R.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            Utf16String::from_utf16(&unescaped),
        )))
    }
}

// 19.2.6.5 Encode ( string, extraUnescaped ), https://tc39.es/ecma262/#sec-encode
fn encode(vm: &Vm, string: Utf16View<'_>, unescaped_set: &str) -> ThrowCompletionOr<Utf16String> {
    // 1. Let strLen be the length of string.
    let string_length = string.length_in_code_units();

    // 2. Let R be the empty String.
    let mut encoded_builder = Vec::with_capacity(string_length);

    // 3. Let alwaysUnescaped be the string-concatenation of the ASCII word characters and "-.!~*'()".
    // 4. Let unescapedSet be the string-concatenation of alwaysUnescaped and extraUnescaped.
    // OPTIMIZATION: We pass in the entire unescapedSet as a StringView to avoid an extra allocation.

    // 5. Let k be 0.
    let mut k = 0;

    // 6. Repeat,
    while k < string_length {
        // a. If k = strLen, return R.
        // Handled below

        // b. Let C be the code unit at index k within string.
        let code_unit = string.code_unit_at(k);
        // c. If C is in unescapedSet, then
        // NOTE: We assume the unescaped set only contains ascii characters as unescaped_set is a StringView.
        if code_unit < 0x80 && unescaped_set.as_bytes().contains(&(code_unit as u8)) {
            // i. Set k to k + 1.
            k += 1;

            // ii. Set R to the string-concatenation of R and C.
            encoded_builder.push(code_unit);
        }
        // d. Else,
        else {
            // i. Let cp be CodePointAt(string, k).
            let code_point = code_point_at(string, k);
            // ii. If cp.[[IsUnpairedSurrogate]] is true, throw a URIError exception.
            if code_point.is_unpaired_surrogate {
                return vm.throw_completion(ErrorKind::URIError, ErrorType::URIMalformed, &[]);
            }

            // iii. Set k to k + cp.[[CodeUnitCount]].
            k += code_point.code_unit_count;

            // iv. Let Octets be the List of octets resulting by applying the UTF-8 transformation to cp.[[CodePoint]].
            // v. For each element octet of Octets, do
            let character = char::from_u32(code_point.code_point).expect("the code point is not a surrogate");
            let mut octets = [0u8; 4];
            for octet in character.encode_utf8(&mut octets).bytes() {
                // 1. Let hex be the String representation of octet, formatted as an uppercase hexadecimal number.
                // 2. Set R to the string-concatenation of R, "%", and ! StringPad(hex, 2𝔽, "0", start).
                append_ascii(&mut encoded_builder, "%");
                append_uppercase_hex(&mut encoded_builder, u32::from(octet), 2);
            }
        }
    }
    Ok(Utf16String::from_utf16(&encoded_builder))
}

fn decode_percent_encoded_byte(vm: &Vm, string: Utf16View<'_>, percent_index: usize) -> ThrowCompletionOr<u8> {
    if percent_index + 2 >= string.length_in_code_units() {
        return vm.throw_completion(ErrorKind::URIError, ErrorType::URIMalformed, &[]);
    }

    let first_digit = string.code_unit_at(percent_index + 1);
    if !is_ascii_hex_digit(first_digit) {
        return vm.throw_completion(ErrorKind::URIError, ErrorType::URIMalformed, &[]);
    }

    let second_digit = string.code_unit_at(percent_index + 2);
    if !is_ascii_hex_digit(second_digit) {
        return vm.throw_completion(ErrorKind::URIError, ErrorType::URIMalformed, &[]);
    }

    Ok(((parse_ascii_hex_digit(first_digit) << 4) | parse_ascii_hex_digit(second_digit)) as u8)
}

// 19.2.6.6 Decode ( string, preserveEscapeSet ), https://tc39.es/ecma262/#sec-decode
// FIXME: Add spec comments to this implementation. It deviates a lot, so that's a bit tricky.
fn decode(vm: &Vm, string: Utf16View<'_>, reserved_set: &str) -> ThrowCompletionOr<Utf16String> {
    let mut decoded_builder = Vec::with_capacity(string.length_in_code_units());
    let mut k = 0;
    while k < string.length_in_code_units() {
        let code_unit = string.code_unit_at(k);
        if code_unit != u16::from(b'%') {
            decoded_builder.push(code_unit);
            k += 1;
            continue;
        }

        let decoded_code_unit = decode_percent_encoded_byte(vm, string, k)?;
        k += 2;

        if decoded_code_unit < 0x80 {
            if reserved_set.as_bytes().contains(&decoded_code_unit) {
                string.substring_view(k - 2, 3).append_to(&mut decoded_builder);
            } else {
                decoded_builder.push(u16::from(decoded_code_unit));
            }
            k += 1;
            continue;
        }

        let leading_ones = (!decoded_code_unit).leading_zeros() as usize;
        if leading_ones == 1 || leading_ones > 4 {
            return vm.throw_completion(ErrorKind::URIError, ErrorType::URIMalformed, &[]);
        }

        let mut utf8_bytes = [decoded_code_unit, 0, 0, 0];
        for utf8_byte in utf8_bytes.iter_mut().take(leading_ones).skip(1) {
            if k + 3 >= string.length_in_code_units() || string.code_unit_at(k + 1) != u16::from(b'%') {
                return vm.throw_completion(ErrorKind::URIError, ErrorType::URIMalformed, &[]);
            }
            *utf8_byte = decode_percent_encoded_byte(vm, string, k + 1)?;
            k += 3;
        }

        let Ok(utf8_view) = core::str::from_utf8(&utf8_bytes[..leading_ones]) else {
            return vm.throw_completion(ErrorKind::URIError, ErrorType::URIMalformed, &[]);
        };
        let mut buffer = [0u16; 2];
        for decoded_code_point in utf8_view.chars() {
            decoded_builder.extend_from_slice(decoded_code_point.encode_utf16(&mut buffer));
        }
        k += 1;
    }
    Ok(Utf16String::from_utf16(&decoded_builder))
}

impl GlobalObject {
    // 19.2.1 eval ( x ), https://tc39.es/ecma262/#sec-eval-x
    pub fn eval(vm: &Vm) -> ThrowCompletionOr<Value> {
        let x = vm.argument(0);

        // 1. Return ? PerformEval(x, false, false).
        perform_eval(vm, x, CallerMode::NonStrict, EvalMode::Indirect)
    }
}
