/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Console.{h,cpp}: the console namespace of https://console.spec.whatwg.org, and the client a host
//! gives it to print with.

use core::cell::Cell;
use std::collections::HashMap;
use std::time::{Duration, Instant};

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::console_log_level::ConsoleLogLevel;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call, length_of_array_like};
use crate::runtime::array::Array;
use crate::runtime::completion::{Throw, ThrowCompletionOr};
use crate::runtime::error_data::ErrorData;
use crate::runtime::object::ShouldThrowExceptions;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::print::{PrintContext, print};
use crate::runtime::property_key::{PropertyKey, StringMayBeNumber};
use crate::utf16::{Utf16View, concatenate, to_utf16_fly_string, utf16_from_wtf8};

pub type LogLevel = ConsoleLogLevel;

#[derive(Clone)]
pub struct Group {
    pub label: Utf16String,
}

pub struct TraceFrame {
    pub function_name: Utf16String,
    pub source_file: Option<Utf16String>,
    pub line: Option<usize>,
    pub column: Option<usize>,
}

#[derive(Default)]
pub struct Trace {
    pub label: Utf16String,
    pub stack: Vec<TraceFrame>,
}

/// ConsoleClient::PrinterArguments.
pub enum PrinterArguments<'vm> {
    Group(Group),
    Trace(Trace),
    Values(MarkedVec<'vm, Value>),
}

// https://console.spec.whatwg.org
#[repr(C)]
#[derive(Trace)]
pub struct Console {
    header: CellHeader,
    realm: Gc<Realm>,
    client: Cell<Option<Gc<ConsoleClient>>>,

    #[gc(untraced)]
    counters: GcRefCell<HashMap<Utf16FlyString, u32>>,
    #[gc(untraced)]
    timer_table: GcRefCell<HashMap<Utf16FlyString, Instant>>,
    #[gc(untraced)]
    group_stack: GcRefCell<Vec<Group>>,
}

define_cell!(Console, Other);

/// The default value the IDL gives the label of count(), countReset(), time(), timeLog() and timeEnd().
const DEFAULT_LABEL: &str = "default";

/// What Utf16String::formatted() makes of a message with a label in it: the label between `before` and `after`.
fn message_with_label(vm: &Vm, before: &str, label: &Utf16String, after: &str) -> Value {
    let (before, after) = (Utf16String::from_utf8(before), Utf16String::from_utf8(after));
    Value::from_string(PrimitiveString::create(
        vm,
        concatenate(&[
            Utf16View::of_string(&before),
            Utf16View::of_string(label),
            Utf16View::of_string(&after),
        ]),
    ))
}

impl Console {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Console> {
        vm.heap().allocate(Console {
            header: CellHeader::for_class(Self::CLASS),
            realm,
            client: Cell::new(None),
            counters: GcRefCell::default(),
            timer_table: GcRefCell::default(),
            group_stack: GcRefCell::default(),
        })
    }

    pub fn set_client(&self, client: Gc<ConsoleClient>) {
        self.client.set(Some(client));
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub fn counters(&self) -> &GcRefCell<HashMap<Utf16FlyString, u32>> {
        &self.counters
    }

    // 1.1.1. assert(condition, ...data), https://console.spec.whatwg.org/#assert
    pub fn assert_(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If condition is true, return.
        let condition = vm.argument(0).to_boolean();
        if condition {
            return Ok(Value::UNDEFINED);
        }

        // 2. Let message be a string without any formatting specifiers indicating generically an assertion failure (such as "Assertion failed").
        let message = PrimitiveString::create_from_fly_string(vm, &Utf16FlyString::from_utf8("Assertion failed"));

        // NOTE: Assemble `data` from the function arguments.
        let data = MarkedVec::new(vm);
        if vm.argument_count() > 1 {
            for i in 1..vm.argument_count() {
                data.push(vm.argument(i));
            }
        }

        // 3. If data is empty, append message to data.
        if data.is_empty() {
            data.push(Value::from_string(message));
        }
        // 4. Otherwise:
        else {
            // 1. Let first be data[0].
            let first = data.get(0).expect("data is not empty");
            // 2. If first is not a String, then prepend message to data.
            if !first.is_string() {
                data.insert(0, Value::from_string(message));
            }
            // 3. Otherwise:
            else {
                // 1. Let concat be the concatenation of message, U+003A (:), U+0020 SPACE, and first.
                let separator = Utf16String::from_utf8(": ");
                let concat = concatenate(&[
                    message.utf16_string_view(),
                    Utf16View::of_string(&separator),
                    first.as_string().utf16_string_view(),
                ]);
                // 2. Set data[0] to concat.
                data.set(0, Value::from_string(PrimitiveString::create(vm, concat)));
            }
        }

        // 5. Perform Logger("assert", data).
        if let Some(client) = self.client.get() {
            client.logger(vm, LogLevel::Assert, &data)?;
        }
        Ok(Value::UNDEFINED)
    }

    // 1.1.2. clear(), https://console.spec.whatwg.org/#clear
    pub fn clear(&self) -> Value {
        // 1. Empty the appropriate group stack.
        self.group_stack.borrow_mut().clear();

        // 2. If possible for the environment, clear the console. (Otherwise, do nothing.)
        if let Some(client) = self.client.get() {
            client.clear();
        }
        Value::UNDEFINED
    }

    // 1.1.3. debug(...data), https://console.spec.whatwg.org/#debug
    pub fn debug(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Perform Logger("debug", data).
        if let Some(client) = self.client.get() {
            let data = self.vm_arguments(vm);
            return client.logger(vm, LogLevel::Debug, &data);
        }
        Ok(Value::UNDEFINED)
    }

    // 1.1.4. error(...data), https://console.spec.whatwg.org/#error
    pub fn error(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Perform Logger("error", data).
        if let Some(client) = self.client.get() {
            let data = self.vm_arguments(vm);
            return client.logger(vm, LogLevel::Error, &data);
        }
        Ok(Value::UNDEFINED)
    }

    // 1.1.5. info(...data), https://console.spec.whatwg.org/#info
    pub fn info(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Perform Logger("info", data).
        if let Some(client) = self.client.get() {
            let data = self.vm_arguments(vm);
            return client.logger(vm, LogLevel::Info, &data);
        }
        Ok(Value::UNDEFINED)
    }

    // 1.1.6. log(...data), https://console.spec.whatwg.org/#log
    pub fn log(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Perform Logger("log", data).
        if let Some(client) = self.client.get() {
            let data = self.vm_arguments(vm);
            return client.logger(vm, LogLevel::Log, &data);
        }
        Ok(Value::UNDEFINED)
    }

    // 1.1.7. table(tabularData, properties), https://console.spec.whatwg.org/#table, WIP
    pub fn table(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        let Some(client) = self.client.get() else {
            return Ok(Value::UNDEFINED);
        };

        if vm.argument_count() > 0 {
            let tabular_data = vm.argument(0);
            let properties_arg = vm.argument(1);

            let properties = PropertyKeySet::new(vm);

            if properties_arg.is_array(vm)? {
                let properties_arr = properties_arg.as_object();
                let properties_length = length_of_array_like(vm, &properties_arr)?;
                for index in 0..properties_length {
                    let value = properties_arr.get(vm, &PropertyKey::from_number(index))?;
                    if !value.is_undefined() {
                        properties.set(PropertyKey::from_value(vm, value)?);
                    }
                }
            }

            // 1. Let `finalRows` be the new list, initially empty
            let final_rows = MarkedVec::new(vm);

            // 2. Let `finalColumns` be the new list, initially empty
            let final_columns = MarkedVec::new(vm);

            let visited_columns = PropertyKeySet::new(vm);

            // 3. If `tabularData` is a list, then:
            if tabular_data.is_array(vm)? {
                let array_like = tabular_data.as_object();

                // 3.1. Let `indices` be get the indices of `tabularData`
                let length = length_of_array_like(vm, &array_like)?;

                // 3.2. For each `index` of `indices`
                for idx in 0..length {
                    let index = PropertyKey::from_number(idx);

                    // 3.2.1. Let `value` be `tabularData[index]`
                    let value = array_like.get(vm, &index)?;

                    // 3.2.2. Perform create table row with `value`, `key`, `finalColumns`, and `properties` that returns `row`
                    let row = create_table_row(
                        vm,
                        self.realm,
                        Value::from_f64(f64::from(index.as_number())),
                        value,
                        &final_columns,
                        &visited_columns,
                        &properties,
                    )?;

                    // 3.2.3. Append `row` to `finalRows`
                    final_rows.push(Value::from_object(row));
                }
            }
            // 4. Otherwise, if `tabularData` is a map, then:
            else if tabular_data.is_object() {
                let object = tabular_data.as_object();
                // 4.1. For each `key` -> `value` of `tabularData`
                // NB: Like C++, this ignores an error that stops the enumeration.
                let _ = object.enumerate_object_properties(vm, |key| -> Option<Throw> {
                    let row = (|| -> ThrowCompletionOr<Gc<Object>> {
                        let index = PropertyKey::from_value(vm, key)?;
                        let value = object.get(vm, &index)?;

                        // 4.1.1. Perform create table row with `key`, `value`, `finalColumns`, and `properties` that returns `row`
                        create_table_row(
                            vm,
                            self.realm,
                            key,
                            value,
                            &final_columns,
                            &visited_columns,
                            &properties,
                        )
                    })();
                    match row {
                        Ok(row) => {
                            // 4.1.2. Append `row` to `finalRows`
                            final_rows.push(Value::from_object(row));
                            None
                        }
                        Err(throw) => Some(throw),
                    }
                });
            }

            // 5. If `finalRows` is not empty, then:
            if !final_rows.is_empty() {
                let table_rows = Array::create_from(vm, self.realm, &final_rows.to_vec());
                let table_cols = Array::create_from(vm, self.realm, &final_columns.to_vec());

                // 5.1. Let `finalData` to be a new map:
                let final_data = Object::create(vm, self.realm, None);

                // 5.2. Set `finalData["rows"]` to `finalRows`
                final_data.set(
                    vm,
                    &vm.names.rows,
                    Value::from_object(table_rows),
                    ShouldThrowExceptions::No,
                )?;

                // 5.3. Set finalData["columns"] to finalColumns
                final_data.set(
                    vm,
                    &vm.names.columns,
                    Value::from_object(table_cols),
                    ShouldThrowExceptions::No,
                )?;

                // 5.4. Perform `Printer("table", finalData)`
                let args = MarkedVec::new(vm);
                args.push(Value::from_object(final_data));
                return client.printer(vm, LogLevel::Table, PrinterArguments::Values(args));
            }
        }

        // 6. Otherwise, perform `Printer("log", tabularData)`
        client.printer(vm, LogLevel::Log, PrinterArguments::Values(self.vm_arguments(vm)))
    }

    // 1.1.8. trace(...data), https://console.spec.whatwg.org/#trace
    pub fn trace(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        let Some(client) = self.client.get() else {
            return Ok(Value::UNDEFINED);
        };

        // 1. Let trace be some implementation-defined, potentially-interactive representation of the callstack from where this function was called.
        let mut trace = Trace::default();
        let stack_trace = vm.stack_trace();

        // NOTE: Skip the first frame (console.trace() itself)
        for element in stack_trace.iter().skip(1) {
            // SAFETY: The contexts of a stack trace are live until the running code returns.
            let context = unsafe { element.execution_context.as_ref() };

            let function_name = context
                .function
                .get()
                .map(|function| function.name_for_call_stack())
                .unwrap_or_default();
            let function_name = if Utf16View::of_string(&function_name).is_empty() {
                Utf16String::from_utf8("<anonymous>")
            } else {
                function_name
            };

            let mut frame = TraceFrame {
                function_name,
                source_file: None,
                line: None,
                column: None,
            };

            if let Some(source_range) = &element.source_range
                && !Utf16View::of_string(source_range.filename()).is_empty()
            {
                frame.source_file = Some(source_range.filename().clone());
                frame.line = Some(source_range.start.line as usize);
                frame.column = Some(source_range.start.column as usize);
            }

            trace.stack.push(frame);
        }

        // 2. Optionally, let formattedData be the result of Formatter(data), and incorporate formattedData as a label for trace.
        if vm.argument_count() > 0 {
            let data = self.vm_arguments(vm);
            let formatted_data = client.formatter(vm, &data)?;
            trace.label = self.value_vector_to_string(vm, &formatted_data)?;
        }

        // 3. Perform Printer("trace", « trace »).
        client.printer(vm, LogLevel::Trace, PrinterArguments::Trace(trace))
    }

    // 1.1.9. warn(...data), https://console.spec.whatwg.org/#warn
    pub fn warn(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Perform Logger("warn", data).
        if let Some(client) = self.client.get() {
            let data = self.vm_arguments(vm);
            return client.logger(vm, LogLevel::Warn, &data);
        }
        Ok(Value::UNDEFINED)
    }

    // 1.1.10. dir(item, options), https://console.spec.whatwg.org/#dir
    pub fn dir(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let object be item with generic JavaScript object formatting applied.
        // NOTE: Generic formatting is performed by ConsoleClient::printer().
        let object = vm.argument(0);

        // 2. Perform Printer("dir", « object », options).
        if let Some(client) = self.client.get() {
            let printer_arguments = MarkedVec::new(vm);
            printer_arguments.push(object);

            return client.printer(vm, LogLevel::Dir, PrinterArguments::Values(printer_arguments));
        }

        Ok(Value::UNDEFINED)
    }

    // 1.1.11 dirxml(...data) https://console.spec.whatwg.org/#dirxml
    pub fn dirxml(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let finalList be a new list, initially empty.
        let final_list = MarkedVec::new(vm);

        // 2. For each item of data:
        for i in 0..vm.argument_count() {
            let item = vm.argument(i);

            // 1. Let converted be a DOM tree representation of item if possible; otherwise let converted be item with
            //    optimally useful formatting applied.
            // FIXME: "Optimally-useful formatting"

            // 2. Append converted to finalList.
            final_list.push(item);
        }

        // 3. Perform Logger("dirxml", finalList).
        if let Some(client) = self.client.get() {
            return client.logger(vm, LogLevel::DirXML, &final_list);
        }

        Ok(Value::UNDEFINED)
    }

    // 1.2.1. count(label), https://console.spec.whatwg.org/#count
    pub fn count(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // NOTE: "default" is the default value in the IDL. https://console.spec.whatwg.org/#ref-for-count
        let label = label_or_fallback(vm, DEFAULT_LABEL)?;
        let key = to_utf16_fly_string(&label);

        // 1. Let map be the associated count map.
        let count = {
            let mut map = self.counters.borrow_mut();

            // 2. If map[label] exists, set map[label] to map[label] + 1.
            // 3. Otherwise, set map[label] to 1.
            let count = map.entry(key).or_insert(0);
            *count = count.wrapping_add(1);
            *count
        };

        // 4. Let concat be the concatenation of label, U+003A (:), U+0020 SPACE, and ToString(map[label]).
        let concat = message_with_label(vm, "", &label, &format!(": {count}"));

        // 5. Perform Logger("count", « concat »).
        let concat_as_vector = MarkedVec::new(vm);
        concat_as_vector.push(concat);
        if let Some(client) = self.client.get() {
            client.logger(vm, LogLevel::Count, &concat_as_vector)?;
        }
        Ok(Value::UNDEFINED)
    }

    // 1.2.2. countReset(label), https://console.spec.whatwg.org/#countreset
    pub fn count_reset(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // NOTE: "default" is the default value in the IDL. https://console.spec.whatwg.org/#ref-for-countreset
        let label = label_or_fallback(vm, DEFAULT_LABEL)?;
        let key = to_utf16_fly_string(&label);

        // 1. Let map be the associated count map.
        // 2. If map[label] exists, set map[label] to 0.
        let found = self
            .counters
            .borrow_mut()
            .get_mut(&key)
            .map(|count| *count = 0)
            .is_some();

        // 3. Otherwise:
        if !found {
            // 1. Let message be a string without any formatting specifiers indicating generically
            //    that the given label does not have an associated count.
            let message = message_with_label(vm, "\"", &label, "\" doesn't have a count");
            // 2. Perform Logger("countReset", « message »);
            let message_as_vector = MarkedVec::new(vm);
            message_as_vector.push(message);
            if let Some(client) = self.client.get() {
                client.logger(vm, LogLevel::CountReset, &message_as_vector)?;
            }
        }

        Ok(Value::UNDEFINED)
    }

    // 1.3.1. group(...data), https://console.spec.whatwg.org/#group
    pub fn group(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let group be a new group.
        // 2. If data is not empty, let groupLabel be the result of Formatter(data).
        let data = self.vm_arguments(vm);
        let group_label = if !data.is_empty() {
            if let Some(client) = self.client.get() {
                let formatted_data = client.formatter(vm, &data)?;
                self.value_vector_to_string(vm, &formatted_data)?
            } else {
                self.value_vector_to_string(vm, &data)?
            }
        }
        // ... Otherwise, let groupLabel be an implementation-chosen label representing a group.
        else {
            Utf16String::from_utf8("Group")
        };

        // 3. Incorporate groupLabel as a label for group.
        let group = Group { label: group_label };

        // 4. Optionally, if the environment supports interactive groups, group should be expanded by default.
        // NOTE: This is handled in Printer.

        // 5. Perform Printer("group", « group »).
        if let Some(client) = self.client.get() {
            client.printer(vm, LogLevel::Group, PrinterArguments::Group(group.clone()))?;
        }

        // 6. Push group onto the appropriate group stack.
        self.group_stack.borrow_mut().push(group);

        Ok(Value::UNDEFINED)
    }

    // 1.3.2. groupCollapsed(...data), https://console.spec.whatwg.org/#groupcollapsed
    pub fn group_collapsed(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let group be a new group.
        // 2. If data is not empty, let groupLabel be the result of Formatter(data).
        let data = self.vm_arguments(vm);
        let group_label = if !data.is_empty() {
            if let Some(client) = self.client.get() {
                let formatted_data = client.formatter(vm, &data)?;
                self.value_vector_to_string(vm, &formatted_data)?
            } else {
                self.value_vector_to_string(vm, &data)?
            }
        }
        // ... Otherwise, let groupLabel be an implementation-chosen label representing a group.
        else {
            Utf16String::from_utf8("Group")
        };

        // 3. Incorporate groupLabel as a label for group.
        let group = Group { label: group_label };

        // 4. Optionally, if the environment supports interactive groups, group should be collapsed by default.
        // NOTE: This is handled in Printer.

        // 5. Perform Printer("groupCollapsed", « group »).
        if let Some(client) = self.client.get() {
            client.printer(vm, LogLevel::GroupCollapsed, PrinterArguments::Group(group.clone()))?;
        }

        // 6. Push group onto the appropriate group stack.
        self.group_stack.borrow_mut().push(group);

        Ok(Value::UNDEFINED)
    }

    // 1.3.3. groupEnd(), https://console.spec.whatwg.org/#groupend
    pub fn group_end(&self) -> Value {
        if self.group_stack.borrow().is_empty() {
            return Value::UNDEFINED;
        }

        // 1. Pop the last group from the group stack.
        self.group_stack.borrow_mut().pop();
        if let Some(client) = self.client.get() {
            client.end_group();
        }

        Value::UNDEFINED
    }

    /// Prints the warning C++ prints for a timer that exists or does not, which no spec has yet: see
    /// https://github.com/whatwg/console/issues/134
    fn print_timer_warning(&self, vm: &Vm, before: &str, label: &Utf16String, after: &str) -> ThrowCompletionOr<()> {
        if let Some(client) = self.client.get() {
            let message_as_vector = MarkedVec::new(vm);
            message_as_vector.push(message_with_label(vm, before, label, after));
            client.printer(vm, LogLevel::Warn, PrinterArguments::Values(message_as_vector))?;
        }
        Ok(())
    }

    // 1.4.1. time(label), https://console.spec.whatwg.org/#time
    pub fn time(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // NOTE: "default" is the default value in the IDL. https://console.spec.whatwg.org/#ref-for-time
        let label = label_or_fallback(vm, DEFAULT_LABEL)?;
        let key = to_utf16_fly_string(&label);

        // 1. If the associated timer table contains an entry with key label, return, optionally reporting
        //    a warning to the console indicating that a timer with label `label` has already been started.
        if self.timer_table.borrow().contains_key(&key) {
            self.print_timer_warning(vm, "Timer '", &label, "' already exists.")?;
            return Ok(Value::UNDEFINED);
        }

        // 2. Otherwise, set the value of the entry with key label in the associated timer table to the current time.
        self.timer_table.borrow_mut().insert(key, Instant::now());
        Ok(Value::UNDEFINED)
    }

    // 1.4.2. timeLog(label, ...data), https://console.spec.whatwg.org/#timelog
    pub fn time_log(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // NOTE: "default" is the default value in the IDL. https://console.spec.whatwg.org/#ref-for-timelog
        let label = label_or_fallback(vm, DEFAULT_LABEL)?;
        let key = to_utf16_fly_string(&label);

        // 1. Let timerTable be the associated timer table.

        // 2. Let startTime be timerTable[label].
        let maybe_start_time = self.timer_table.borrow().get(&key).copied();

        // NOTE: Warn if the timer doesn't exist. Not part of the spec yet, but discussed here: https://github.com/whatwg/console/issues/134
        let Some(start_time) = maybe_start_time else {
            self.print_timer_warning(vm, "Timer '", &label, "' does not exist.")?;
            return Ok(Value::UNDEFINED);
        };

        // 3. Let duration be a string representing the difference between the current time and startTime, in an implementation-defined format.
        let duration = human_readable_time(start_time.elapsed());

        // 4. Let concat be the concatenation of label, U+003A (:), U+0020 SPACE, and duration.
        let concat = message_with_label(vm, "", &label, &format!(": {duration}"));

        // 5. Prepend concat to data.
        let data = MarkedVec::with_capacity(vm, vm.argument_count());
        data.push(concat);
        for i in 1..vm.argument_count() {
            data.push(vm.argument(i));
        }

        // 6. Perform Printer("timeLog", data).
        if let Some(client) = self.client.get() {
            client.printer(vm, LogLevel::TimeLog, PrinterArguments::Values(data))?;
        }
        Ok(Value::UNDEFINED)
    }

    // 1.4.3. timeEnd(label), https://console.spec.whatwg.org/#timeend
    pub fn time_end(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // NOTE: "default" is the default value in the IDL. https://console.spec.whatwg.org/#ref-for-timeend
        let label = label_or_fallback(vm, DEFAULT_LABEL)?;
        let key = to_utf16_fly_string(&label);

        // 1. Let timerTable be the associated timer table.

        // 2. Let startTime be timerTable[label].
        let maybe_start_time = self.timer_table.borrow().get(&key).copied();

        // NOTE: Warn if the timer doesn't exist. Not part of the spec yet, but discussed here: https://github.com/whatwg/console/issues/134
        let Some(start_time) = maybe_start_time else {
            self.print_timer_warning(vm, "Timer '", &label, "' does not exist.")?;
            return Ok(Value::UNDEFINED);
        };

        // 3. Remove timerTable[label].
        self.timer_table.borrow_mut().remove(&key);

        // 4. Let duration be a string representing the difference between the current time and startTime, in an implementation-defined format.
        let duration = human_readable_time(start_time.elapsed());

        // 5. Let concat be the concatenation of label, U+003A (:), U+0020 SPACE, and duration.
        let concat = message_with_label(vm, "", &label, &format!(": {duration}"));

        // 6. Perform Printer("timeEnd", « concat »).
        if let Some(client) = self.client.get() {
            let concat_as_vector = MarkedVec::new(vm);
            concat_as_vector.push(concat);
            client.printer(vm, LogLevel::TimeEnd, PrinterArguments::Values(concat_as_vector))?;
        }
        Ok(Value::UNDEFINED)
    }

    pub fn vm_arguments<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Value> {
        let arguments = MarkedVec::with_capacity(vm, vm.argument_count());
        for i in 0..vm.argument_count() {
            arguments.push(vm.argument(i));
        }
        arguments
    }

    /// Writes what a host's debug output shows of a message, with dbgln(), which writes to the standard error.
    pub fn output_debug_message(&self, log_level: LogLevel, output: Utf16View<'_>) {
        let prefix = match log_level {
            LogLevel::Debug => "\x1b[32;1m(js debug)\x1b[0m",
            LogLevel::Error => "\x1b[32;1m(js error)\x1b[0m",
            LogLevel::Info => "\x1b[32;1m(js info)\x1b[0m",
            LogLevel::Log => "\x1b[32;1m(js log)\x1b[0m",
            LogLevel::Warn => "\x1b[32;1m(js warn)\x1b[0m",
            _ => "\x1b[32;1m(js)\x1b[0m",
        };
        eprintln!("{prefix} {}", output.to_utf8());
    }

    pub fn report_exception(
        &self,
        name: Utf16View<'_>,
        message: Utf16View<'_>,
        error_data: &ErrorData,
        in_promise: bool,
    ) {
        if let Some(client) = self.client.get() {
            client.report_exception(name, message, error_data, in_promise);
        }
    }

    fn value_vector_to_string(&self, vm: &Vm, values: &MarkedVec<'_, Value>) -> ThrowCompletionOr<Utf16String> {
        let mut builder: Vec<u16> = Vec::new();

        for index in 0..values.len() {
            let item = values.get(index).expect("the index is in bounds");
            if !builder.is_empty() {
                builder.push(u16::from(b' '));
            }

            Utf16View::of_string(&item.to_utf16_string(vm)?).append_to(&mut builder);
        }

        Ok(Utf16String::from_utf16(&builder))
    }
}

/// A set of property keys, which keeps the symbols among them alive.
struct PropertyKeySet<'vm> {
    keys: MarkedVec<'vm, PropertyKey>,
}

impl<'vm> PropertyKeySet<'vm> {
    fn new(vm: &'vm Vm) -> Self {
        Self {
            keys: MarkedVec::new(vm),
        }
    }

    fn contains(&self, key: &PropertyKey) -> bool {
        (0..self.keys.len()).any(|index| self.keys.get(index).as_ref() == Some(key))
    }

    fn set(&self, key: PropertyKey) {
        if !self.contains(&key) {
            self.keys.push(key);
        }
    }

    fn size(&self) -> usize {
        self.keys.len()
    }
}

// To [create table row] given tabularDataItem, rowIndex, list finalColumns, and optional list properties, perform the following steps:
fn create_table_row(
    vm: &Vm,
    realm: Gc<Realm>,
    row_index: Value,
    tabular_data_item: Value,
    final_columns: &MarkedVec<'_, Value>,
    visited_columns: &PropertyKeySet<'_>,
    properties: &PropertyKeySet<'_>,
) -> ThrowCompletionOr<Gc<Object>> {
    let add_column = |column_name: &PropertyKey| {
        // In order to not iterate over the final_columns to find if a column is
        // already in the list, an additional hash map is used to identify
        // if a column is already visited without needing to loop through the whole
        // array.
        if !visited_columns.contains(column_name) {
            visited_columns.set(column_name.clone());

            if column_name.is_string() {
                final_columns.push(Value::from_string(PrimitiveString::create_from_fly_string(
                    vm,
                    column_name.as_string(),
                )));
            } else if column_name.is_symbol() {
                final_columns.push(Value::from_symbol(column_name.as_symbol()));
            } else if column_name.is_number() {
                final_columns.push(Value::from_f64(f64::from(column_name.as_number())));
            }
        }
    };

    // 1. Let `row` be a new map
    let row = Object::create(vm, realm, None);

    // 2. Set `row["(index)"]` to `rowIndex`
    {
        let key = PropertyKey::from_fly_string(Utf16FlyString::from_utf8("(index)"), StringMayBeNumber::No);
        row.set(vm, &key, row_index, ShouldThrowExceptions::No)?;

        add_column(&key);
    }

    // 3. If `tabularDataItem` is a list, then:
    if tabular_data_item.is_array(vm)? {
        let array_like = tabular_data_item.as_object();

        // 3.1. Let `indices` be get the indices of `tabularDataItem`
        let length = length_of_array_like(vm, &array_like)?;

        // 3.2. For each `index` of `indices`
        for i in 0..length {
            let key = PropertyKey::from_number(i);

            // 3.2.1. Let `value` be `tabularDataItem[index]`
            let value = array_like.get(vm, &key)?;

            // 3.2.2. If `properties` is not empty and `properties` does not contain `index`, continue
            if properties.size() > 0 && !properties.contains(&key) {
                continue;
            }

            // 3.2.3. Set `row[index]` to `value`
            row.set(vm, &key, value, ShouldThrowExceptions::No)?;

            // 3.2.4. If `finalColumns` does not contain `index`, append `index` to `finalColumns`
            add_column(&key);
        }
    }
    // 4. Otherwise, if `tabularDataItem` is a map, then:
    else if tabular_data_item.is_object() {
        let object = tabular_data_item.as_object();
        // 4.1. For each `key` -> `value` of `tabularDataItem`
        // NB: Like C++, this ignores an error that stops the enumeration.
        let _ = object.enumerate_object_properties(vm, |key_v| -> Option<Throw> {
            let step = || -> ThrowCompletionOr<()> {
                let key = PropertyKey::from_value(vm, key_v)?;

                // 4.1.1. If `properties` is not empty and `properties` does not contain `key`, continue
                if properties.size() > 0 && !properties.contains(&key) {
                    return Ok(());
                }

                // 4.1.2. Set `row[key]` to `value`
                let value = object.get(vm, &key)?;
                row.set(vm, &key, value, ShouldThrowExceptions::No)?;

                // 4.1.3. If `finalColumns` does not contain `key`, append `key` to `finalColumns`
                add_column(&key);

                Ok(())
            };
            step().err()
        });
    }
    // 5. Otherwise,
    else {
        // 5.1. Set `row["Value"]` to `tabularDataItem`
        row.set(vm, &vm.names.Value, tabular_data_item, ShouldThrowExceptions::No)?;

        // 5.2. If `finalColumns` does not contain "Value", append "Value" to `finalColumns`
        add_column(&vm.names.Value);
    }

    // 6. Return row
    Ok(row)
}

fn label_or_fallback(vm: &Vm, fallback: &str) -> ThrowCompletionOr<Utf16String> {
    if vm.argument_count() > 0 && !vm.argument(0).is_undefined() {
        vm.argument(0).to_utf16_string(vm)
    } else {
        Ok(Utf16String::from_utf8(fallback))
    }
}

/// AK::human_readable_time(), which shows the seconds as `{:.3}` does: with at most three fraction digits, rounded
/// the way AK rounds them and without trailing zeros. Like Duration::to_milliseconds(), it rounds the duration up to
/// whole milliseconds first.
fn human_readable_time(duration: Duration) -> String {
    let mut milliseconds = i64::try_from(duration.as_nanos().div_ceil(1_000_000)).unwrap_or(i64::MAX);

    let days = milliseconds / 86_400_000;
    milliseconds %= 86_400_000;

    let hours = milliseconds / 3_600_000;
    milliseconds %= 3_600_000;

    let minutes = milliseconds / 60_000;
    milliseconds %= 60_000;

    #[allow(clippy::cast_precision_loss, reason = "the milliseconds of a minute fit in a double")]
    let seconds = milliseconds as f64 / 1000.0;

    let mut builder = String::new();

    let plural = |count: i64| if count == 1 { "" } else { "s" };
    if days > 0 {
        builder.push_str(&format!("{days} day{} ", plural(days)));
    }

    if hours > 0 {
        builder.push_str(&format!("{hours} hour{} ", plural(hours)));
    }

    if minutes > 0 {
        builder.push_str(&format!("{minutes} minute{} ", plural(minutes)));
    }

    builder.push_str(&format!(
        "{} second{}",
        format_with_at_most_three_fraction_digits(seconds),
        if seconds == 1.0 { "" } else { "s" }
    ));

    builder
}

/// AK's FormatBuilder::put_f64_with_precision() for a non-negative value, a precision of 3 and the default display
/// mode.
#[allow(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the value is non-negative and small"
)]
fn format_with_at_most_three_fraction_digits(mut value: f64) -> String {
    const PRECISION: usize = 3;

    let mut integer_value = value as u64;
    value -= (value as i64) as f64;

    let mut fraction_digits: Vec<u8> = Vec::new();

    let mut epsilon = 0.5;
    for _ in 0..PRECISION {
        epsilon /= 10.0;
    }

    for _ in 0..PRECISION {
        if value - ((value as i64) as f64) < epsilon {
            break;
        }

        value *= 10.0;
        epsilon *= 10.0;

        if value > f64::from(u32::MAX) {
            value -= ((value as u64) - ((value as u64) % 10)) as f64;
        }

        fraction_digits.push(b'0' + ((value as u32) % 10) as u8);
    }

    // Round up if the following decimal is 5 or higher
    if ((value * 10.0) as u64) % 10 >= 5 {
        let mut carries_into_integer = true;
        for digit in fraction_digits.iter_mut().rev() {
            if *digit == b'9' {
                *digit = b'0';
            } else {
                *digit += 1;
                carries_into_integer = false;
                break;
            }
        }
        if carries_into_integer {
            integer_value += 1;
        }
    }

    while fraction_digits.last() == Some(&b'0') {
        fraction_digits.pop();
    }

    let mut output = integer_value.to_string();
    if !fraction_digits.is_empty() {
        output.push('.');
        output.push_str(core::str::from_utf8(&fraction_digits).expect("the digits are ASCII"));
    }
    output
}

/// The virtual methods of ConsoleClient, which the hosts' clients override.
pub struct ConsoleClientMethods {
    // 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
    pub printer: for<'vm> fn(&ConsoleClient, &'vm Vm, LogLevel, PrinterArguments<'vm>) -> ThrowCompletionOr<Value>,
    pub add_css_style_to_current_message: fn(&ConsoleClient, Utf16View<'_>),
    pub report_exception: fn(&ConsoleClient, Utf16View<'_>, Utf16View<'_>, &ErrorData, bool),
    pub clear: fn(&ConsoleClient),
    pub end_group: fn(&ConsoleClient),
}

#[repr(C)]
#[derive(Trace)]
pub struct ConsoleClient {
    header: CellHeader,
    #[gc(untraced)]
    methods: &'static ConsoleClientMethods,
    console: Gc<Console>,
}

define_cell!(ConsoleClient, Other);

impl ConsoleClient {
    /// ConsoleClient(Console&), for a client whose class is `class` and whose virtual methods are `methods`.
    pub fn new(
        class: &'static crate::gc::class::Class,
        methods: &'static ConsoleClientMethods,
        console: Gc<Console>,
    ) -> Self {
        Self {
            header: CellHeader::for_class(class),
            methods,
            console,
        }
    }

    pub fn console(&self) -> Gc<Console> {
        self.console
    }

    pub fn printer<'vm>(
        &self,
        vm: &'vm Vm,
        log_level: LogLevel,
        arguments: PrinterArguments<'vm>,
    ) -> ThrowCompletionOr<Value> {
        (self.methods.printer)(self, vm, log_level, arguments)
    }

    pub fn add_css_style_to_current_message(&self, style: Utf16View<'_>) {
        (self.methods.add_css_style_to_current_message)(self, style);
    }

    pub fn report_exception(
        &self,
        name: Utf16View<'_>,
        message: Utf16View<'_>,
        error_data: &ErrorData,
        in_promise: bool,
    ) {
        (self.methods.report_exception)(self, name, message, error_data, in_promise);
    }

    pub fn clear(&self) {
        (self.methods.clear)(self);
    }

    pub fn end_group(&self) {
        (self.methods.end_group)(self);
    }

    // 2.1. Logger(logLevel, args), https://console.spec.whatwg.org/#logger
    pub fn logger(&self, vm: &Vm, log_level: LogLevel, args: &MarkedVec<'_, Value>) -> ThrowCompletionOr<Value> {
        // 1. If args is empty, return.
        if args.is_empty() {
            return Ok(Value::UNDEFINED);
        }

        // 2. Let first be args[0].
        let first = args.get(0).expect("args is not empty");

        // 3. Let rest be all elements following first in args.
        let rest_size = args.len() - 1;

        // 4. If rest is empty, perform Printer(logLevel, « first ») and return.
        if rest_size == 0 {
            let first_as_vector = MarkedVec::new(vm);
            first_as_vector.push(first);
            return self.printer(vm, log_level, PrinterArguments::Values(first_as_vector));
        }
        // 5. Otherwise, perform Printer(logLevel, Formatter(args)).
        let formatted = self.formatter(vm, args)?;
        self.printer(vm, log_level, PrinterArguments::Values(formatted))?;

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 2.2. Formatter(args), https://console.spec.whatwg.org/#formatter
    pub fn formatter<'vm>(&self, vm: &'vm Vm, args: &MarkedVec<'_, Value>) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        let realm = self.console.realm();

        // 1. If args’s size is 1, return args.
        if args.len() == 1 {
            return Ok(copy_of(vm, args));
        }

        // 2. Let target be the first element of args.
        let mut target = match args.get(0) {
            Some(first) if first.is_string() => first.as_string().utf16_string(),
            _ => Utf16String::default(),
        };

        // 3. Let current be the second element of args.
        let current = if args.len() > 1 {
            args.get(1).expect("args has a second element")
        } else {
            Value::UNDEFINED
        };

        // 4. Find the first possible format specifier specifier, from the left to the right in target.
        let find_specifier = |target: Utf16View<'_>| -> Option<(usize, u16)> {
            let length = target.length_in_code_units();
            let mut start_index = 0;
            while start_index < length {
                let index = (start_index..length).find(|&index| target.code_unit_at(index) == u16::from(b'%'))?;
                if index + 1 >= length {
                    return None;
                }

                let specifier = target.code_unit_at(index + 1);
                if b"cdfioOs".iter().any(|&character| u16::from(character) == specifier) {
                    return Some((index, specifier));
                }

                start_index = index + 1;
            }
            None
        };
        let maybe_specifier = find_specifier(Utf16View::of_string(&target));

        // 5. If no format specifier was found, return args.
        let Some((specifier_index, specifier)) = maybe_specifier else {
            return Ok(copy_of(vm, args));
        };
        // 6. Otherwise:
        let specifier = u8::try_from(specifier).expect("the specifier is ASCII");
        let converted: Option<Value>;

        // 1. If specifier is %s, let converted be the result of Call(%String%, undefined, « current »).
        if specifier == b's' {
            converted = Some(call(
                vm,
                Value::from_object(realm.intrinsics().string_constructor(vm)),
                Value::UNDEFINED,
                &[current],
            )?);
        }
        // 2. If specifier is %d or %i:
        else if specifier == b'd' || specifier == b'i' {
            // 1. If current is a Symbol, let converted be NaN
            if current.is_symbol() {
                converted = Some(Value::from_f64(f64::NAN));
            }
            // 2. Otherwise, let converted be the result of Call(%parseInt%, undefined, « current, 10 »).
            else {
                converted = Some(call(
                    vm,
                    Value::from_object(realm.intrinsics().parse_int_function()),
                    Value::UNDEFINED,
                    &[current, Value::from_i32(10)],
                )?);
            }
        }
        // 3. If specifier is %f:
        else if specifier == b'f' {
            // 1. If current is a Symbol, let converted be NaN
            if current.is_symbol() {
                converted = Some(Value::from_f64(f64::NAN));
            }
            // 2. Otherwise, let converted be the result of Call(% parseFloat %, undefined, « current »).
            else {
                converted = Some(call(
                    vm,
                    Value::from_object(realm.intrinsics().parse_float_function()),
                    Value::UNDEFINED,
                    &[current],
                )?);
            }
        }
        // 4. If specifier is %o, optionally let converted be current with optimally useful formatting applied.
        else if specifier == b'o' {
            // FIXME: "Optimally-useful formatting"
            converted = Some(current);
        }
        // 5. If specifier is %O, optionally let converted be current with generic JavaScript object formatting applied.
        else if specifier == b'O' {
            // TODO: "generic JavaScript object formatting"
            converted = Some(current);
        }
        // 6. TODO: process %c
        else {
            // NOTE: This has no spec yet. `%c` specifiers treat the argument as CSS styling for the log message.
            let css_style = current.to_utf16_string(vm)?;
            self.add_css_style_to_current_message(Utf16View::of_string(&css_style));
            converted = Some(Value::from_string(PrimitiveString::create(vm, Utf16String::default())));
        }

        // 7. If any of the previous steps set converted, replace specifier in target with converted.
        if let Some(converted) = converted {
            let converted_string = converted.to_utf16_string(vm)?;
            let target_view = Utf16View::of_string(&target);
            let replaced = concatenate(&[
                target_view.substring_view(0, specifier_index),
                Utf16View::of_string(&converted_string),
                target_view.substring_view(
                    specifier_index + 2,
                    target_view.length_in_code_units() - specifier_index - 2,
                ),
            ]);
            target = replaced;
        }

        // 7. Let result be a list containing target together with the elements of args starting from the third onward.
        let result = MarkedVec::with_capacity(vm, args.len() - 1);
        result.push(Value::from_string(PrimitiveString::create(vm, target)));
        for i in 2..args.len() {
            result.push(args.get(i).expect("the index is in bounds"));
        }

        // 8. Return Formatter(result).
        self.formatter(vm, &result)
    }

    pub fn generically_format_values(&self, vm: &Vm, values: &MarkedVec<'_, Value>) -> ThrowCompletionOr<Utf16String> {
        let mut builder: Vec<u8> = Vec::new();
        let mut first = true;
        for index in 0..values.len() {
            let value = values.get(index).expect("the index is in bounds");
            if !first {
                builder.push(b' ');
            }
            let mut context = PrintContext {
                vm,
                stream: &mut builder,
                strip_ansi: true,
                raw_strings: false,
            };
            print(value, &mut context).expect("printing into a buffer succeeds");
            first = false;
        }
        Ok(utf16_from_wtf8(&builder).map_or_else(
            || Utf16String::from_utf8(&String::from_utf8_lossy(&builder)),
            |code_units| Utf16String::from_utf16(&code_units),
        ))
    }
}

fn copy_of<'vm>(vm: &'vm Vm, values: &MarkedVec<'_, Value>) -> MarkedVec<'vm, Value> {
    let copy = MarkedVec::with_capacity(vm, values.len());
    for index in 0..values.len() {
        copy.push(values.get(index).expect("the index is in bounds"));
    }
    copy
}
