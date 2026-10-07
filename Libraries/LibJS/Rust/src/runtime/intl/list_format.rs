/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::{Completion, Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::iterator::{IteratorHint, get_iterator, iterator_close, iterator_step_value};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::unicode::intl::{self as unicode, ListFormatPartition, ListFormatType, Style};
use crate::utf16::Utf16View;

/// 14 ListFormat Objects, https://tc39.es/ecma402/#listformat-objects
#[repr(C)]
#[derive(Trace)]
pub struct ListFormat {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    type_: Cell<ListFormatType>, // [[Type]]
    #[gc(untraced)]
    style: Cell<Style>, // [[Style]]

    // Non-standard. Stores the ICU list formatter for the Intl object's formatting options.
    #[gc(untraced)]
    formatter: GcRefCell<Option<unicode::ListFormat>>,
}

define_cell!(ListFormat, Object, extends: [Object], finalize: finalize);

impl Deref for ListFormat {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for ListFormat {
    fn finalize(&self) {
        drop(self.formatter.replace(None));
    }
}

impl ListFormat {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> ListFormat {
        ListFormat {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            type_: Cell::new(ListFormatType::Conjunction),
            style: Cell::new(Style::Long),
            formatter: GcRefCell::new(None),
        }
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn type_(&self) -> ListFormatType {
        self.type_.get()
    }

    pub fn set_type(&self, type_: Utf16View<'_>) {
        self.type_.set(unicode::list_format_type_from_string(type_));
    }

    pub fn type_string(&self) -> &'static str {
        unicode::list_format_type_to_string(self.type_.get())
    }

    pub fn style(&self) -> Style {
        self.style.get()
    }

    pub fn set_style(&self, style: Utf16View<'_>) {
        self.style.set(unicode::style_from_string(style));
    }

    pub fn style_string(&self) -> &'static str {
        unicode::style_to_string(self.style.get())
    }

    /// Calls `callback` with the ICU list formatter, which must not allocate or call into the VM.
    pub fn with_formatter<R>(&self, callback: impl FnOnce(&unicode::ListFormat) -> R) -> R {
        let formatter = self.formatter.borrow();
        callback(
            formatter
                .as_ref()
                .expect("the Intl.ListFormat constructor creates the ICU list formatter"),
        )
    }

    pub fn set_formatter(&self, formatter: unicode::ListFormat) {
        self.formatter.replace(Some(formatter));
    }
}

impl IntlObject for ListFormat {
    // 14.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.ListFormat-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « ».
        &[]
    }

    // 14.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.ListFormat-internal-slots
    fn resolution_option_descriptors<'vm>(&self, _: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « ».
        Vec::new()
    }
}

// 14.5.2 CreatePartsFromList ( listFormat, list ), https://tc39.es/ecma402/#sec-createpartsfromlist
pub fn create_parts_from_list(list_format: &ListFormat, list: &[Utf16String]) -> Vec<ListFormatPartition> {
    list_format.with_formatter(|formatter| formatter.format_to_parts(list))
}

// 14.5.3 FormatList ( listFormat, list ), https://tc39.es/ecma402/#sec-formatlist
pub fn format_list(list_format: &ListFormat, list: &[Utf16String]) -> Utf16String {
    // 1. Let parts be ! CreatePartsFromList(listFormat, list).
    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    list_format.with_formatter(|formatter| formatter.format(list))
}

// 14.5.4 FormatListToParts ( listFormat, list ), https://tc39.es/ecma402/#sec-formatlisttoparts
pub fn format_list_to_parts(vm: &Vm, list_format: &ListFormat, list: &[Utf16String]) -> Gc<Array> {
    let realm = vm.current_realm().expect("FormatListToParts runs in a realm");

    // 1. Let parts be ! CreatePartsFromList(listFormat, list).
    let parts = create_parts_from_list(list_format, list);

    // 2. Let result be ! ArrayCreate(0).
    let result = Array::create(vm, realm, 0, None).must();

    // 3. Let n be 0.
    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (n, part) in parts.into_iter().enumerate() {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        let object = Object::create(vm, realm, Some(realm.object_prototype()));

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.type_,
                Value::from_string(PrimitiveString::create(vm, part.type_)),
            )
            .must();

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.value,
                Value::from_string(PrimitiveString::create(vm, part.value)),
            )
            .must();

        // d. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        result
            .create_data_property_or_throw(vm, &PropertyKey::from_number(n as u64), Value::from_object(object))
            .must();

        // e. Increment n by 1.
    }

    // 5. Return result.
    result
}

// 14.5.5 StringListFromIterable ( iterable ), https://tc39.es/ecma402/#sec-createstringlistfromiterable
pub fn string_list_from_iterable(vm: &Vm, iterable: Value) -> ThrowCompletionOr<Vec<Utf16String>> {
    // 1. If iterable is undefined, then
    if iterable.is_undefined() {
        // a. Return a new empty List.
        return Ok(Vec::new());
    }

    // 2. Let iteratorRecord be ? GetIterator(iterable, sync).
    let iterator_record = get_iterator(vm, iterable, IteratorHint::Sync)?;

    // 3. Let list be a new empty List.
    let mut list: Vec<Utf16String> = Vec::new();

    // 4. Repeat,
    loop {
        // a. Let next be ? IteratorStepValue(iteratorRecord).
        let next = iterator_step_value(vm, &iterator_record)?;

        // b. If next is DONE, then
        let Some(next) = next else {
            // a. Return list.
            return Ok(list);
        };

        // c. If Type(next) is not String, then
        if !next.is_string() {
            // 1. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAString, &[&next]);

            // 2. Return ? IteratorClose(iteratorRecord, error).
            return Err(iterator_close(vm, &iterator_record, Completion::from(error)).release_error());
        }

        // iii. Append next to list.
        list.push(next.as_string().utf16_string());
    }
}
