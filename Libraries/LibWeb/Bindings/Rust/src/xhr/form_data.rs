/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FormData, https://xhr.spec.whatwg.org/#interface-formdata

use core::cell;
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{DeferGc, GcRefCell, MarkedVec, RawNativeFunctionResult, Trace, Vm};

use crate::bindings::conversions::{CppInterface, UsvString};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_from_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::file_api::blob::{AnyBlob, BlobPropertyBag, EndingType};
use crate::file_api::file::{File, FilePropertyBag};

unsafe extern "C" {
    fn web_form_data_create_from_form(
        realm: *mut c_void,
        form: *mut c_void,
        submitter: *mut c_void,
        out_form_data: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
}

/// https://xhr.spec.whatwg.org/#formdataentryvalue
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum FormDataEntryValue {
    File(Ref<File>),
    USVString(UsvString),
}

/// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#form-entry
pub(crate) type Entry = (UsvString, FormDataEntryValue);

pub(crate) enum StringOrBlob {
    String(Utf16String),
    Blob(AnyBlob),
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#create-an-entry
pub(crate) fn create_an_entry(vm: &Vm, name: &Utf16String, value: StringOrBlob, filename: Option<UsvString>) -> Entry {
    // 1. Set name to the result of converting name into a scalar value string.
    let name = UsvString::from_utf16_string(name);

    let value = match value {
        // 2. If value is a string, then set value to the result of converting value into a scalar value string.
        StringOrBlob::String(string) => FormDataEntryValue::USVString(UsvString::from_utf16_string(&string)),
        // 3. Otherwise:
        StringOrBlob::Blob(blob) => {
            // 1. If value is not a File object, then set value to a new File object, representing the same bytes, whose
            //    name attribute value is "blob".
            let file = match blob {
                AnyBlob::File(file) => file,
                AnyBlob::Blob(blob) => {
                    let options = file_property_bag(blob.type_string().clone(), None);
                    let file = File::create(blob.raw_bytes().to_vec(), Utf16String::from_ascii(b"blob"), &options);
                    allocate(vm, |_| file)
                }
            };

            // 2. If filename is given, then set value to a new File object, representing the same bytes, whose name
            //    attribute is filename.
            let file = match filename {
                Some(filename) => {
                    let blob = file.parent();
                    let options = file_property_bag(blob.type_string().clone(), Some(file.last_modified_value()));
                    let file = File::create(blob.raw_bytes().to_vec(), filename.into_utf16_string(), &options);
                    allocate(vm, |_| file)
                }
                None => file,
            };

            FormDataEntryValue::File(file)
        }
    };

    // 4. Return an entry whose name is name and whose value is value.
    (name, value)
}

fn file_property_bag(r#type: Utf16String, last_modified: Option<i64>) -> FilePropertyBag {
    FilePropertyBag {
        parent: BlobPropertyBag {
            endings: EndingType::Transparent,
            r#type,
        },
        last_modified,
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FormData {
    // https://xhr.spec.whatwg.org/#concept-formdata-entry-list
    entry_list: GcRefCell<Vec<Entry>>,
}

impl Wrappable for FormData {}

impl FormData {
    pub(crate) fn new(_: &DeferGc<'_>, entry_list: Vec<Entry>) -> Self {
        Self {
            entry_list: GcRefCell::new(entry_list),
        }
    }

    pub(crate) fn entry_list(&self) -> cell::Ref<'_, Vec<Entry>> {
        self.entry_list.borrow()
    }

    // https://xhr.spec.whatwg.org/#dom-formdata
    pub(crate) fn construct(
        cx: &mut CallContext,
        form: Option<CppInterface>,
        submitter: Option<CppInterface>,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. If form is given, then:
        // NB: C++ runs the substeps and creates the object.
        let Some(form) = form else {
            return Ok(allocate(cx.vm(), |defer_gc| Self::new(defer_gc, Vec::new())));
        };
        let submitter = submitter.map_or(core::ptr::null_mut(), |submitter| submitter.wrapper().as_ptr().cast());
        let mut form_data = core::ptr::null_mut();
        // SAFETY: The realm and the wrappers are live.
        unsafe {
            web_form_data_create_from_form(
                cx.realm().as_ptr().cast(),
                form.wrapper().as_ptr().cast(),
                submitter,
                &mut form_data,
            )
        }
        .into_completion()?;
        // SAFETY: C++ creates a FormData when it does not throw.
        Ok(unsafe { cell_from_abi(form_data) })
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-append
    pub(crate) fn append(&self, cx: &mut CallContext, name: UsvString, value: UsvString) -> WebIdlResult<()> {
        // 1. Let value be value if given; otherwise blobValue.
        // 2. Let entry be the result of creating an entry with name, value, and filename if given.
        let entry = create_an_entry(
            cx.vm(),
            name.as_utf16_string(),
            StringOrBlob::String(value.into_utf16_string()),
            None,
        );

        // 3. Append entry to this’s entry list.
        self.entry_list.borrow_mut(cx.vm()).push(entry);
        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-append-blob
    pub(crate) fn append_blob(
        &self,
        cx: &mut CallContext,
        name: UsvString,
        blob_value: AnyBlob,
        filename: Option<UsvString>,
    ) -> WebIdlResult<()> {
        // 1. Let value be value if given; otherwise blobValue.
        // 2. Let entry be the result of creating an entry with name, value, and filename if given.
        let entry = create_an_entry(
            cx.vm(),
            name.as_utf16_string(),
            StringOrBlob::Blob(blob_value),
            filename,
        );

        // 3. Append entry to this’s entry list.
        self.entry_list.borrow_mut(cx.vm()).push(entry);
        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-delete
    pub(crate) fn delete(&self, cx: &mut CallContext, name: UsvString) -> WebIdlResult<()> {
        // The delete(name) method steps are to remove all entries whose name is name from this’s entry list.
        self.entry_list
            .borrow_mut(cx.vm())
            .retain(|(entry_name, _)| *entry_name != name);
        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-get
    pub(crate) fn get(&self, _: &mut CallContext, name: UsvString) -> WebIdlResult<Option<FormDataEntryValue>> {
        // 1. If there is no entry whose name is name in this’s entry list, then return null.
        // 2. Return the value of the first entry whose name is name from this’s entry list.
        Ok(self.value_named(&name))
    }

    pub(crate) fn value_named(&self, name: &UsvString) -> Option<FormDataEntryValue> {
        self.entry_list
            .borrow()
            .iter()
            .find(|(entry_name, _)| entry_name == name)
            .map(|(_, value)| value.clone())
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-getall
    pub(crate) fn get_all<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        name: UsvString,
    ) -> WebIdlResult<MarkedVec<'vm, FormDataEntryValue>> {
        // 1. If there is no entry whose name is name in this’s entry list, then return the empty list.
        // 2. Return the values of all entries whose name is name, in order, from this’s entry list.
        let values = MarkedVec::new(cx.vm());
        for (_, value) in self
            .entry_list
            .borrow()
            .iter()
            .filter(|(entry_name, _)| *entry_name == name)
        {
            values.push(value.clone());
        }
        Ok(values)
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-has
    pub(crate) fn has(&self, _: &mut CallContext, name: UsvString) -> WebIdlResult<bool> {
        // The has(name) method steps are to return true if there is an entry whose name is name in this’s entry list; otherwise false.
        Ok(self.value_named(&name).is_some())
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-set
    pub(crate) fn set(&self, cx: &mut CallContext, name: UsvString, value: UsvString) -> WebIdlResult<()> {
        // 1. Let value be value if given; otherwise blobValue.
        // 2. Let entry be the result of creating an entry with name, value, and filename if given.
        let entry = create_an_entry(
            cx.vm(),
            name.as_utf16_string(),
            StringOrBlob::String(value.into_utf16_string()),
            None,
        );
        self.set_entry(cx.vm(), entry);
        Ok(())
    }

    // https://xhr.spec.whatwg.org/#dom-formdata-set-blob
    pub(crate) fn set_blob(
        &self,
        cx: &mut CallContext,
        name: UsvString,
        blob_value: AnyBlob,
        filename: Option<UsvString>,
    ) -> WebIdlResult<()> {
        // 1. Let value be value if given; otherwise blobValue.
        // 2. Let entry be the result of creating an entry with name, value, and filename if given.
        let entry = create_an_entry(
            cx.vm(),
            name.as_utf16_string(),
            StringOrBlob::Blob(blob_value),
            filename,
        );
        self.set_entry(cx.vm(), entry);
        Ok(())
    }

    fn set_entry(&self, vm: &Vm, entry: Entry) {
        let mut entry_list = self.entry_list.borrow_mut(vm);

        // 3. If there are entries in this’s entry list whose name is name, then replace the first such entry with entry
        //    and remove the others.
        if let Some(first) = entry_list.iter().position(|(entry_name, _)| *entry_name == entry.0) {
            let mut index = 0;
            entry_list.retain(|(entry_name, _)| {
                let keep = index <= first || *entry_name != entry.0;
                index += 1;
                keep
            });
            entry_list[first] = entry;
        }
        // 4. Otherwise, append entry to this’s entry list.
        else {
            entry_list.push(entry);
        }
    }
}

// NB: The conversion of a FormDataEntryValue to a JS value is in the generated bindings.
#[cfg(web_idl_bindings)]
impl crate::bindings::iterable::PairIterable for FormData {
    type Key = UsvString;
    type Value = FormDataEntryValue;

    fn value_pairs_to_iterate_over(&self) -> impl core::ops::Deref<Target = [(UsvString, FormDataEntryValue)]> + '_ {
        // The value pairs to iterate over are this’s entry list’s entries with the key being the name and the value
        // being the value.
        cell::Ref::map(self.entry_list.borrow(), Vec::as_slice)
    }
}
