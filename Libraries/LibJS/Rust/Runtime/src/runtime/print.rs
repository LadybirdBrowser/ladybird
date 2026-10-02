/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Pretty printing of values for js-rust, mirroring Libraries/LibJS/Print.cpp.

use std::collections::HashSet;
use std::io::{self, Write};

use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::async_generator::AsyncGenerator;
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::ecmascript_function_object::EcmascriptFunctionObject;
use crate::runtime::error::Error;
use crate::runtime::generator_object::GeneratorObject;
use crate::runtime::map::Map;
use crate::runtime::native_function::NativeFunction;
use crate::runtime::number_object::NumberObject;
use crate::runtime::promise::{Promise, PromiseState};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::proxy_object::ProxyObject;
use crate::runtime::regexp_object::RegExpObject;
use crate::runtime::set::Set;
use crate::runtime::shared_function_instance_data::FunctionKind;
use crate::runtime::string_object::StringObject;
use crate::runtime::weak_map::WeakMap;
use crate::runtime::weak_ref::WeakRef;
use crate::runtime::weak_set::WeakSet;
use crate::utf16::Utf16View;

/// Where and how to print. A Vec<u8> stream stands in for the StringBuilder C++ can print into.
pub struct PrintContext<'a> {
    pub vm: &'a Vm,
    pub stream: &'a mut dyn Write,
    pub strip_ansi: bool,
    pub raw_strings: bool,
}

fn escape_for_string_literal(string: Utf16View<'_>) -> Vec<u16> {
    let mut builder = Vec::with_capacity(string.length_in_code_units());
    for code_unit in string.code_units() {
        let escape = match code_unit {
            0x0D => b'r',
            0x0B => b'v',
            0x0C => b'f',
            0x08 => b'b',
            0x0A => b'n',
            0x5C => b'\\',
            _ => {
                builder.push(code_unit);
                continue;
            }
        };
        builder.push(u16::from(b'\\'));
        builder.push(u16::from(escape));
    }
    builder
}

/// The GC::RootHashTable<GC::Ref<JS::Object>> of the objects printed so far. The objects stay rooted while printing
/// runs getters, so that no object allocated meanwhile can take the address of one that was printed.
struct SeenObjects<'vm> {
    objects: MarkedVec<'vm, Gc<Object>>,
    addresses: HashSet<usize>,
}

impl<'vm> SeenObjects<'vm> {
    fn new(vm: &'vm Vm) -> Self {
        Self {
            objects: MarkedVec::new(vm),
            addresses: HashSet::new(),
        }
    }

    fn contains(&self, object: Gc<Object>) -> bool {
        self.addresses.contains(&object.as_ptr().addr())
    }

    fn set(&mut self, object: Gc<Object>) {
        if self.addresses.insert(object.as_ptr().addr()) {
            self.objects.push(object);
        }
    }

    fn size(&self) -> usize {
        self.addresses.len()
    }
}

fn strip_ansi(format_string: &str) -> Vec<u8> {
    let format_string = format_string.as_bytes();
    if format_string.is_empty() {
        return Vec::new();
    }

    let mut builder = Vec::with_capacity(format_string.len());
    let mut i = 0;
    while i < format_string.len() - 1 {
        if format_string[i] == 0x1B && format_string[i + 1] == b'[' {
            while i < format_string.len() && format_string[i] != b'm' {
                i += 1;
            }
        } else {
            builder.push(format_string[i]);
        }
        i += 1;
    }
    if i < format_string.len() {
        builder.push(format_string[i]);
    }
    builder
}

/// js_out() with a format string that has no arguments. Like C++, stripping ANSI colors only applies to the format
/// string, so the arguments of a format string are written with the functions below rather than through this.
fn js_out(print_context: &mut PrintContext<'_>, format_string: &str) -> io::Result<()> {
    if print_context.strip_ansi {
        return print_context.stream.write_all(&strip_ansi(format_string));
    }
    print_context.stream.write_all(format_string.as_bytes())
}

fn js_out_argument(print_context: &mut PrintContext<'_>, argument: &str) -> io::Result<()> {
    print_context.stream.write_all(argument.as_bytes())
}

fn js_out_utf16_argument(print_context: &mut PrintContext<'_>, argument: Utf16View<'_>) -> io::Result<()> {
    print_context.stream.write_all(&argument.to_wtf8())
}

fn print_type(print_context: &mut PrintContext<'_>, name: &str) -> io::Result<()> {
    js_out(print_context, "[\x1b[36;1m")?;
    js_out_argument(print_context, name)?;
    js_out(print_context, "\x1b[0m]")
}

fn print_utf16_type(print_context: &mut PrintContext<'_>, name: Utf16View<'_>) -> io::Result<()> {
    js_out(print_context, "[\x1b[36;1m")?;
    js_out_utf16_argument(print_context, name)?;
    js_out(print_context, "\x1b[0m]")
}

fn print_separator(print_context: &mut PrintContext<'_>, first: &mut bool) -> io::Result<()> {
    js_out_argument(print_context, if *first { " " } else { ", " })?;
    *first = false;
    Ok(())
}

fn print_array(
    print_context: &mut PrintContext<'_>,
    array: Gc<Object>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    js_out(print_context, "[")?;
    let mut first = true;
    let mut printed_count: usize = 0;
    let mut i: u32 = 0;
    while i < array.indexed_array_like_size() {
        if !array.indexed_has(i) {
            i += 1;
            continue;
        }
        print_separator(print_context, &mut first)?;
        let value_or_error = array.get(print_context.vm, &PropertyKey::from(i));
        // The V8 repl doesn't throw an exception here, and instead just
        // prints 'undefined'. We may choose to replicate that behavior in
        // the future, but for now lets just catch the error
        let Ok(value) = value_or_error else {
            return Ok(());
        };
        print_value(print_context, value, seen_objects)?;
        printed_count += 1;
        if printed_count > 100 && i + 1 < array.indexed_array_like_size() {
            js_out(print_context, ", ...")?;
            break;
        }
        i += 1;
    }
    if !first {
        js_out(print_context, " ")?;
    }
    js_out(print_context, "]")
}

fn print_object(
    print_context: &mut PrintContext<'_>,
    object: Gc<Object>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    js_out_argument(print_context, object.class().class_name())?;
    js_out(print_context, "{")?;
    let mut first = true;
    const MAX_NUMBER_OF_NEW_OBJECTS: usize = 20; // Arbitrary limit
    let original_num_seen_objects = seen_objects.size();

    let vm = print_context.vm;
    let maybe_completion = object.enumerate_object_properties(vm, |property_key| -> Option<()> {
        // The V8 repl doesn't throw an exception on accessing properties, and instead just
        // prints 'undefined'. We may choose to replicate that behavior in
        // the future, but for now lets just catch the error
        if print_separator(print_context, &mut first).is_err() {
            return Some(());
        }
        if js_out(print_context, "\x1b[33;1m").is_err() {
            return Some(());
        }
        // NOTE: Ignore this error to always print out "reset" ANSI sequence
        let _ = print_value(print_context, property_key, seen_objects);
        if js_out(print_context, "\x1b[0m: ").is_err() {
            return Some(());
        }
        let Ok(maybe_property_key) = PropertyKey::from_value(vm, property_key) else {
            return Some(());
        };
        let Ok(value) = object.get(vm, &maybe_property_key) else {
            return Some(());
        };
        let error = print_value(print_context, value, seen_objects);
        // FIXME: Come up with a better way to structure the data so that we don't care about this limit
        if seen_objects.size() > original_num_seen_objects + MAX_NUMBER_OF_NEW_OBJECTS {
            return Some(()); // Stop once we've seen a ton of objects, to prevent spamming the console.
        }
        if error.is_err() {
            return Some(());
        }
        None
    });
    // Swallow Error/undefined from printing properties
    if !matches!(maybe_completion, Ok(None)) {
        return Ok(());
    }

    if !first {
        js_out(print_context, " ")?;
    }
    js_out(print_context, "}")
}

fn print_function(print_context: &mut PrintContext<'_>, function_object: Gc<Object>) -> io::Result<()> {
    let ecmascript_function_object = function_object.downcast::<EcmascriptFunctionObject>();
    if let Some(ecmascript_function_object) = ecmascript_function_object {
        match ecmascript_function_object.kind() {
            FunctionKind::Normal => print_type(print_context, "Function")?,
            FunctionKind::Generator => print_type(print_context, "GeneratorFunction")?,
            FunctionKind::Async => print_type(print_context, "AsyncFunction")?,
            FunctionKind::AsyncGenerator => print_type(print_context, "AsyncGeneratorFunction")?,
        }
    } else {
        print_type(print_context, function_object.class().class_name())?;
    }
    if let Some(ecmascript_function_object) = ecmascript_function_object {
        js_out(print_context, " ")?;
        js_out_utf16_argument(
            print_context,
            Utf16View::of_fly_string(&ecmascript_function_object.name()),
        )?;
    } else if let Some(native_function) = function_object.downcast::<NativeFunction>() {
        js_out(print_context, " ")?;
        js_out_utf16_argument(print_context, Utf16View::of_fly_string(&native_function.name()))?;
    }
    Ok(())
}

fn print_error(
    print_context: &mut PrintContext<'_>,
    object: Gc<Object>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    let vm = print_context.vm;
    let name = object.get_without_side_effects(vm, &vm.names.name);
    let message = object.get_without_side_effects(vm, &vm.names.message);
    if name.is_accessor() || message.is_accessor() {
        // NB: The object is among the seen objects already, so this prints it as an already printed object.
        print_value(print_context, Value::from_object(object), seen_objects)?;
    } else {
        let name_string = name.to_utf16_string_without_side_effects();
        let message_string = message.to_utf16_string_without_side_effects();
        print_utf16_type(print_context, Utf16View::of_string(&name_string))?;
        if !Utf16View::of_string(&message_string).is_empty() {
            js_out(print_context, " \x1b[31;1m")?;
            js_out_utf16_argument(print_context, Utf16View::of_string(&message_string))?;
            js_out(print_context, "\x1b[0m")?;
        }
    }
    Ok(())
}

fn print_map(print_context: &mut PrintContext<'_>, map: Gc<Map>, seen_objects: &mut SeenObjects<'_>) -> io::Result<()> {
    print_type(print_context, "Map")?;
    js_out(print_context, " {")?;
    let mut first = true;
    let iterator = map.begin();
    while !iterator.is_end() {
        let entry = iterator.current();
        print_separator(print_context, &mut first)?;
        print_value(print_context, entry.key, seen_objects)?;
        js_out(print_context, " => ")?;
        print_value(print_context, entry.value, seen_objects)?;
        iterator.advance();
    }
    if !first {
        js_out(print_context, " ")?;
    }
    js_out(print_context, "}")
}

fn print_set(print_context: &mut PrintContext<'_>, set: Gc<Set>, seen_objects: &mut SeenObjects<'_>) -> io::Result<()> {
    print_type(print_context, "Set")?;
    js_out(print_context, " {")?;
    let mut first = true;
    let iterator = set.begin();
    while !iterator.is_end() {
        let value = iterator.current();
        print_separator(print_context, &mut first)?;
        print_value(print_context, value, seen_objects)?;
        iterator.advance();
    }
    if !first {
        js_out(print_context, " ")?;
    }
    js_out(print_context, "}")
}

fn print_weak_map(print_context: &mut PrintContext<'_>, weak_map: Gc<WeakMap>) -> io::Result<()> {
    print_type(print_context, "WeakMap")?;
    js_out(print_context, " (")?;
    js_out_argument(print_context, &weak_map.weak_map_size().to_string())?;
    // Note: We could tell you what's actually inside, but not in insertion order.
    js_out(print_context, ")")
}

fn print_weak_set(print_context: &mut PrintContext<'_>, weak_set: Gc<WeakSet>) -> io::Result<()> {
    print_type(print_context, "WeakSet")?;
    js_out(print_context, " (")?;
    js_out_argument(print_context, &weak_set.weak_set_size().to_string())?;
    // Note: We could tell you what's actually inside, but not in insertion order.
    js_out(print_context, ")")
}

fn print_weak_ref(
    print_context: &mut PrintContext<'_>,
    weak_ref: Gc<WeakRef>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    print_type(print_context, "WeakRef")?;
    js_out(print_context, " ")?;
    let value = weak_ref.value();
    print_value(
        print_context,
        if value.is_empty() { Value::UNDEFINED } else { value },
        seen_objects,
    )
}

fn print_boolean_object(
    print_context: &mut PrintContext<'_>,
    boolean_object: Gc<BooleanObject>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    print_type(print_context, "Boolean")?;
    js_out(print_context, " ")?;
    print_value(print_context, Value::from_bool(boolean_object.boolean()), seen_objects)
}

fn print_number_object(
    print_context: &mut PrintContext<'_>,
    number_object: Gc<NumberObject>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    print_type(print_context, "Number")?;
    js_out(print_context, " ")?;
    print_value(print_context, Value::from_f64(number_object.number()), seen_objects)
}

fn print_promise(
    print_context: &mut PrintContext<'_>,
    promise: Gc<Promise>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    print_type(print_context, "Promise")?;
    match promise.state() {
        PromiseState::Pending => {
            js_out(print_context, "\n  state: ")?;
            js_out(print_context, "\x1b[36;1mPending\x1b[0m")?;
        }
        PromiseState::Fulfilled => {
            js_out(print_context, "\n  state: ")?;
            js_out(print_context, "\x1b[32;1mFulfilled\x1b[0m")?;
            js_out(print_context, "\n  result: ")?;
            print_value(print_context, promise.result(), seen_objects)?;
        }
        PromiseState::Rejected => {
            js_out(print_context, "\n  state: ")?;
            js_out(print_context, "\x1b[31;1mRejected\x1b[0m")?;
            js_out(print_context, "\n  result: ")?;
            print_value(print_context, promise.result(), seen_objects)?;
        }
    }
    Ok(())
}

fn print_proxy_object(
    print_context: &mut PrintContext<'_>,
    proxy_object: Gc<ProxyObject>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    print_type(print_context, "Proxy")?;
    js_out(print_context, "\n  target: ")?;
    print_value(print_context, Value::from_object(proxy_object.target()), seen_objects)?;
    js_out(print_context, "\n  handler: ")?;
    print_value(print_context, Value::from_object(proxy_object.handler()), seen_objects)
}

fn print_regexp_object(print_context: &mut PrintContext<'_>, regexp_object: Gc<RegExpObject>) -> io::Result<()> {
    print_type(print_context, "RegExp")?;
    js_out(print_context, " \x1b[34;1m/")?;
    js_out_utf16_argument(
        print_context,
        Utf16View::of_string(&regexp_object.escape_regexp_pattern()),
    )?;
    js_out(print_context, "/")?;
    js_out_utf16_argument(print_context, Utf16View::of_string(&regexp_object.flags()))?;
    js_out(print_context, "\x1b[0m")
}

fn print_generator(print_context: &mut PrintContext<'_>, generator: Gc<Object>) -> io::Result<()> {
    print_type(print_context, generator.class().class_name())
}

fn print_async_generator(print_context: &mut PrintContext<'_>, generator: Gc<Object>) -> io::Result<()> {
    print_type(print_context, generator.class().class_name())
}

fn is_error_object(object: Gc<Object>) -> bool {
    object.is::<Error>()
}

/// &prototype == prototype.shape().realm().intrinsics().error_prototype(): whether `prototype` is the %Error.prototype%
/// of the realm it was made in.
fn is_error_prototype_of_its_realm(vm: &Vm, prototype: Gc<Object>) -> bool {
    prototype == prototype.shape().realm().intrinsics().error_prototype(vm)
}

fn print_string_object(
    print_context: &mut PrintContext<'_>,
    string_object: Gc<StringObject>,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    print_type(print_context, "String")?;
    js_out(print_context, " ")?;
    print_value(
        print_context,
        Value::from_string(string_object.primitive_string()),
        seen_objects,
    )
}

fn print_value(
    print_context: &mut PrintContext<'_>,
    value: Value,
    seen_objects: &mut SeenObjects<'_>,
) -> io::Result<()> {
    if value.is_empty() {
        js_out(print_context, "\x1b[34;1m<empty>\x1b[0m")?;
        return Ok(());
    }

    if value.is_object() {
        if seen_objects.contains(value.as_object()) {
            // FIXME: Maybe we should only do this for circular references,
            //        not for all reoccurring objects.
            js_out(print_context, "<already printed Object ")?;
            js_out_argument(print_context, &format!("{:#018x}", value.as_object().as_ptr().addr()))?;
            js_out(print_context, ">")?;
            return Ok(());
        }
        seen_objects.set(value.as_object());
    }

    if value.is_object() {
        let object = value.as_object();
        if object.is::<Array>() {
            return print_array(print_context, object, seen_objects);
        }
        if object.is_function() {
            return print_function(print_context, object);
        }
        // NB: Date objects are printed by print_date() here, before errors, once the runtime has them.
        if is_error_object(object) {
            return print_error(print_context, object, seen_objects);
        }

        let prototype_or_error = object.internal_get_prototype_of(print_context.vm);
        if let Ok(Some(prototype)) = prototype_or_error
            && is_error_prototype_of_its_realm(print_context.vm, prototype)
        {
            return print_error(print_context, object, seen_objects);
        }

        if let Some(regexp_object) = object.downcast::<RegExpObject>() {
            return print_regexp_object(print_context, regexp_object);
        }
        if let Some(map) = object.downcast::<Map>() {
            return print_map(print_context, map, seen_objects);
        }
        if let Some(set) = object.downcast::<Set>() {
            return print_set(print_context, set, seen_objects);
        }
        if let Some(weak_map) = object.downcast::<WeakMap>() {
            return print_weak_map(print_context, weak_map);
        }
        if let Some(weak_set) = object.downcast::<WeakSet>() {
            return print_weak_set(print_context, weak_set);
        }
        if let Some(weak_ref) = object.downcast::<WeakRef>() {
            return print_weak_ref(print_context, weak_ref, seen_objects);
        }
        // NB: Then DataView, which the runtime does not have yet.
        if let Some(proxy_object) = object.downcast::<ProxyObject>() {
            return print_proxy_object(print_context, proxy_object, seen_objects);
        }
        if let Some(promise) = object.downcast::<Promise>() {
            return print_promise(print_context, promise, seen_objects);
        }
        // NB: After Promise, Print.cpp checks for ArrayBuffer, then GeneratorObject below.
        if object.is::<GeneratorObject>() {
            return print_generator(print_context, object);
        }
        if object.is::<AsyncGenerator>() {
            return print_async_generator(print_context, object);
        }
        // NB: After AsyncGenerator, Print.cpp checks for the typed arrays (object.is_typed_array()), then
        //     BooleanObject, NumberObject and StringObject below.
        if let Some(boolean_object) = object.downcast::<BooleanObject>() {
            return print_boolean_object(print_context, boolean_object, seen_objects);
        }
        if let Some(number_object) = object.downcast::<NumberObject>() {
            return print_number_object(print_context, number_object, seen_objects);
        }
        if let Some(string_object) = object.downcast::<StringObject>() {
            return print_string_object(print_context, string_object, seen_objects);
        }
        // NB: Print.cpp then checks for Intl.DisplayNames, Intl.Locale, Intl.ListFormat, Intl.NumberFormat,
        //     Intl.DateTimeFormat, Intl.RelativeTimeFormat, Intl.PluralRules, Intl.Collator, Intl.Segmenter, Segments,
        //     Intl.DurationFormat, and Temporal.Duration, Temporal.Instant, Temporal.PlainDate, Temporal.PlainDateTime,
        //     Temporal.PlainMonthDay, Temporal.PlainTime, Temporal.PlainYearMonth and Temporal.ZonedDateTime.
        //     Everything else is printed as an ordinary object.
        return print_object(print_context, object, seen_objects);
    }

    if value.is_string() {
        js_out(print_context, "\x1b[32;1m")?;
    } else if value.is_number() || value.is_bigint() {
        js_out(print_context, "\x1b[35;1m")?;
    } else if value.is_boolean() || value.is_null() {
        js_out(print_context, "\x1b[33;1m")?;
    } else if value.is_undefined() {
        js_out(print_context, "\x1b[34;1m")?;
    }

    if value.is_string() && !print_context.raw_strings {
        js_out(print_context, "\"")?;
    } else if value.is_negative_zero() {
        js_out(print_context, "-")?;
    }

    let contents = value.to_utf16_string_without_side_effects();
    if value.is_string() && !print_context.raw_strings {
        let escaped = escape_for_string_literal(Utf16View::of_string(&contents));
        js_out_utf16_argument(print_context, Utf16View::Utf16(&escaped))?;
    } else {
        js_out_utf16_argument(print_context, Utf16View::of_string(&contents))?;
    }

    if value.is_string() && !print_context.raw_strings {
        js_out(print_context, "\"")?;
    }
    js_out(print_context, "\x1b[0m")
}

pub fn print(value: Value, print_context: &mut PrintContext<'_>) -> io::Result<()> {
    let mut seen_objects = SeenObjects::new(print_context.vm);
    print_value(print_context, value, &mut seen_objects)
}
