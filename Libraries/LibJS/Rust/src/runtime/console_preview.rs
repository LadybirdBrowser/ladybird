/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ops::ControlFlow;

use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::date::Date;
use crate::runtime::date_prototype::to_date_string;
use crate::runtime::ecmascript_function_object::EcmascriptFunctionObject;
use crate::runtime::error::Error;
use crate::runtime::map::Map;
use crate::runtime::native_function::NativeFunction;
use crate::runtime::number_object::NumberObject;
use crate::runtime::promise::{Promise, PromiseState};
use crate::runtime::set::Set;
use crate::runtime::shared_function_instance_data::FunctionKind;
use crate::runtime::string_object::StringObject;
use crate::utf16::Utf16View;

const MAX_BYTES: usize = 8192;
const MAX_STRING_LENGTH: usize = 1024;
const MAX_ITEMS: usize = 10;
const MAX_DEPTH: usize = 2;

// Console output is a preview. Read stored values directly, without property lookup, so logging can't run getters
// or proxy traps. Limits apply before reading children or converting strings, including for terminal output.
struct Preview<'a> {
    vm: &'a Vm,
    text: String,
    truncated: bool,
}

impl Preview<'_> {
    fn full(&self) -> bool {
        self.truncated || self.text.len() >= MAX_BYTES
    }

    fn append(&mut self, text: &str) {
        if self.full() {
            return;
        }
        let mut end = text.len().min(MAX_BYTES - self.text.len());
        while !text.is_char_boundary(end) {
            end -= 1;
        }
        self.text.push_str(&text[..end]);
        self.truncated = end < text.len();
    }

    fn string(&mut self, text: Utf16View<'_>, quoted: bool) {
        if quoted {
            self.append("\"");
        }
        let length = text.length_in_code_units().min(MAX_STRING_LENGTH);
        for code_point in text.substring_view(0, length).to_utf8().chars() {
            if self.full() {
                break;
            }
            match code_point {
                '\n' if quoted => self.append("\\n"),
                '\r' if quoted => self.append("\\r"),
                '\t' if quoted => self.append("\\t"),
                '\\' if quoted => self.append("\\\\"),
                '"' if quoted => self.append("\\\""),
                _ => self.append(code_point.encode_utf8(&mut [0; 4])),
            }
        }
        if length < text.length_in_code_units() {
            self.append("...");
        }
        if quoted {
            self.append("\"");
        }
    }

    fn value(&mut self, value: Value, depth: usize, raw_strings: bool) {
        if self.full() {
            return;
        }
        if value.is_accessor() {
            self.append("[Getter/Setter]");
            return;
        }
        if value.is_empty() {
            self.append("<empty>");
            return;
        }
        if value.is_string() {
            self.string(Utf16View::of_string(&value.as_string().utf16_string()), !raw_strings);
            return;
        }
        if value.is_bigint() && value.as_bigint().big_integer().bits() > 4096 {
            self.append("BigInt {...}");
            return;
        }
        if value.is_symbol() {
            self.append("Symbol(");
            if let Some(description) = value.as_symbol().description() {
                self.string(Utf16View::of_string(description), false);
            }
            self.append(")");
            return;
        }
        if value.is_number() && value.as_f64() == 0.0 && value.as_f64().is_sign_negative() {
            self.append("-0");
            return;
        }
        if !value.is_object() {
            self.string(
                Utf16View::of_string(&value.to_utf16_string_without_side_effects()),
                false,
            );
            return;
        }

        let object = value.as_object();

        // Built-ins whose value lives in an internal slot show that value, which reading the slot can't change.
        if object.is_function() {
            let name = if let Some(function) = object.downcast::<EcmascriptFunctionObject>() {
                self.append(match function.kind() {
                    FunctionKind::Normal => "Function",
                    FunctionKind::Generator => "GeneratorFunction",
                    FunctionKind::Async => "AsyncFunction",
                    FunctionKind::AsyncGenerator => "AsyncGeneratorFunction",
                });
                Some(function.name())
            } else {
                self.append(object.class().class_name());
                object.downcast::<NativeFunction>().map(|function| function.name())
            };
            if let Some(name) = name
                && !name.is_empty()
            {
                self.append(" ");
                self.string(Utf16View::of_fly_string(&name), false);
            }
            return;
        }
        if let Some(boolean_object) = object.downcast::<BooleanObject>() {
            self.append("Boolean ");
            self.value(Value::from_bool(boolean_object.boolean()), depth + 1, false);
            return;
        }
        if let Some(number_object) = object.downcast::<NumberObject>() {
            self.append("Number ");
            self.value(Value::from_f64(number_object.number()), depth + 1, false);
            return;
        }
        if let Some(string_object) = object.downcast::<StringObject>() {
            self.append("String ");
            self.value(Value::from_string(string_object.primitive_string()), depth + 1, false);
            return;
        }

        self.append(object.class().class_name());
        if object.is::<super::proxy_object::ProxyObject>() {
            return;
        }
        if let Some(date) = object.downcast::<Date>() {
            self.append(" ");
            self.string(Utf16View::of_string(&to_date_string(date.date_value())), false);
            return;
        }

        if depth >= MAX_DEPTH {
            self.append(" {...}");
            return;
        }

        if let Some(promise) = object.downcast::<Promise>() {
            match promise.state() {
                PromiseState::Pending => self.append(" {<pending>}"),
                PromiseState::Fulfilled | PromiseState::Rejected => {
                    let state = if promise.state() == PromiseState::Fulfilled {
                        "fulfilled"
                    } else {
                        "rejected"
                    };
                    self.append(&format!(" {{<{state}>: "));
                    self.value(promise.result(), depth + 1, false);
                    self.append("}");
                }
            }
            return;
        }

        if object.is::<Array>() {
            self.append(" [");
            let length = object.indexed_array_like_size();
            for index in 0..length.min(MAX_ITEMS as u32) {
                if self.full() {
                    break;
                }
                if index > 0 {
                    self.append(", ");
                }
                if let Some(entry) = object.indexed_get(index) {
                    self.value(entry.value, depth + 1, false);
                } else {
                    self.append("<empty>");
                }
            }
            if length > MAX_ITEMS as u32 {
                self.append(", ...");
            }
            self.append("]");
            return;
        }

        if let Some(map) = object.downcast::<Map>() {
            self.append(" {");
            let iterator = map.begin();
            for index in 0..MAX_ITEMS {
                if iterator.is_end() || self.full() {
                    break;
                }
                if index > 0 {
                    self.append(", ");
                }
                let entry = iterator.current();
                self.value(entry.key, depth + 1, false);
                self.append(" => ");
                self.value(entry.value, depth + 1, false);
                iterator.advance();
            }
            if !iterator.is_end() {
                self.append(", ...");
            }
            self.append("}");
            return;
        }

        if let Some(set) = object.downcast::<Set>() {
            self.append(" {");
            let iterator = set.begin();
            for index in 0..MAX_ITEMS {
                if iterator.is_end() || self.full() {
                    break;
                }
                if index > 0 {
                    self.append(", ");
                }
                self.value(iterator.current(), depth + 1, false);
                iterator.advance();
            }
            if !iterator.is_end() {
                self.append(", ...");
            }
            self.append("}");
            return;
        }

        // An error leads with its message, so that the properties stored ahead of it can't push it past the item limit.
        let message_key = &self.vm.names.message;
        let is_error = object.is::<Error>();
        let mut message_shown = false;
        if is_error && let Some(metadata) = object.shape().lookup(message_key) {
            let message = object.get_direct(metadata.offset);
            if message.is_string() {
                self.append(": ");
                self.string(Utf16View::of_string(&message.as_string().utf16_string()), false);
                message_shown = true;
            }
        }

        self.append(" {");
        let mut count = 0;
        let indexed_length = object.indexed_array_like_size();
        for index in 0..indexed_length.min(MAX_ITEMS as u32) {
            if self.full() {
                break;
            }
            if let Some(entry) = object.indexed_get(index) {
                if count > 0 {
                    self.append(", ");
                }
                self.append(&format!("{index}: "));
                self.value(entry.value, depth + 1, false);
                count += 1;
            }
        }

        // Keep the bounded set of preview keys on the stack, where the collector can see any symbol keys.
        let remaining = MAX_ITEMS - count;
        let mut properties = [const { None }; MAX_ITEMS];
        let mut property_count = 0;
        object.shape().for_each_property_in_insertion_order(|key, metadata| {
            if property_count == remaining {
                return ControlFlow::Break(());
            }
            if message_shown && key == message_key {
                return ControlFlow::Continue(());
            }
            properties[property_count] = Some((key.clone(), metadata.offset));
            property_count += 1;
            ControlFlow::Continue(())
        });
        for (key, offset) in properties.iter().flatten() {
            if self.full() {
                break;
            }
            if count > 0 {
                self.append(", ");
            }
            if key.is_symbol() {
                self.value(Value::from_symbol(key.as_symbol()), depth + 1, false);
            } else {
                self.string(Utf16View::of_string(&key.to_utf16_string()), true);
            }
            self.append(": ");
            self.value(object.get_direct(*offset), depth + 1, false);
            count += 1;
        }
        if object.shape().property_count() as usize > property_count + usize::from(message_shown)
            || indexed_length > MAX_ITEMS as u32
        {
            if count > 0 {
                self.append(", ");
            }
            self.append("...");
        }
        self.append("}");
    }
}

pub(super) fn format(vm: &Vm, value: Value, raw_strings: bool) -> String {
    let mut preview = Preview {
        vm,
        text: String::new(),
        truncated: false,
    };
    preview.value(value, 0, raw_strings);
    if preview.full() {
        preview.text.push_str("...");
    }
    preview.text
}
