/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! DataTransferItem, https://html.spec.whatwg.org/multipage/dnd.html#the-datatransferitem-interface

use core::cell::{self, Cell};

use ak::Utf16String;
use libjs_rust::api::{PrimitiveString, Trace, Value, Vm};

use super::data_transfer::DataTransfer;
use super::drag_data_store::{DragDataStore, Mode};
use crate::bindings::callback::CallbackFunction;
use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{TaskSource, queue_a_task};
use crate::bindings::{CallContext, WebIdlResult};
use crate::file_api::file::File;

#[derive(Trace)]
pub(crate) struct DataTransferItem {
    data_transfer: Ref<DataTransfer>,
    index: Cell<Option<usize>>,
}

impl Wrappable for DataTransferItem {}

impl DataTransferItem {
    pub(super) fn create(vm: &Vm, data_transfer: Ref<DataTransfer>, index: usize) -> Ref<Self> {
        allocate(vm, |_| Self {
            data_transfer,
            index: Cell::new(Some(index)),
        })
    }

    pub(super) fn set_index(&self, index: Option<usize>) {
        self.index.set(index);
    }

    fn store_and_index(&self) -> Option<(cell::Ref<'_, DragDataStore>, usize)> {
        let index = self.index.get()?;
        Some((self.data_transfer.store()?, index))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitem-kind
    pub(crate) fn kind(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The kind attribute must return the empty string if the DataTransferItem object is in the disabled mode; otherwise
        // it must return the string given in the cell from the second column of the following table from the row whose cell
        // in the first column contains the drag data item kind of the item represented by the DataTransferItem object:
        //
        //     Kind | String
        //     ---------------
        //     Text | "string"
        //     File | "file"
        let Some((store, index)) = self.store_and_index() else {
            return Ok(Utf16String::default());
        };
        let (is_file, _, _) = store.item(index);
        Ok(Utf16String::from_ascii(if is_file { b"file" } else { b"string" }))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitem-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The type attribute must return the empty string if the DataTransferItem object is in the disabled mode; otherwise
        // it must return the drag data item type string of the item represented by the DataTransferItem object.
        let Some((store, index)) = self.store_and_index() else {
            return Ok(Utf16String::default());
        };
        let (_, type_string, _) = store.item(index);
        Ok(Utf16String::from(type_string))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitem-getasstring
    pub(crate) fn get_as_string(&self, cx: &mut CallContext, callback: Option<CallbackFunction>) -> WebIdlResult<()> {
        // 1. If the callback is null, return.
        let Some(callback) = callback else {
            return Ok(());
        };

        // 2. If the DataTransferItem object is not in the read/write mode or the read-only mode, return. The callback is
        //    never invoked.
        let Some((store, index)) = self
            .store_and_index()
            .filter(|(store, _)| store.mode() != Mode::Protected)
        else {
            return Ok(());
        };

        // 3. If the drag data item kind is not text, then return. The callback is never invoked.
        let (is_file, _, data) = store.item(index);
        if is_file {
            return Ok(());
        }

        // 4. Otherwise, queue a task to invoke callback, passing the actual data of the item represented by the
        //    DataTransferItem object as the argument.
        queue_a_task(
            cx.vm(),
            TaskSource::Unspecified,
            (callback, data),
            |vm, (callback, data)| {
                let data = Value::from_string(PrimitiveString::create(vm, data));
                // AD-HOC: The spec gives no exception behavior, which WebIDL requires for this callback.
                //         Report exceptions.
                callback.invoke_and_report(&[data]);
            },
        );
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitem-getasfile
    pub(crate) fn get_as_file(&self, cx: &mut CallContext) -> WebIdlResult<Option<Ref<File>>> {
        // 1. If the DataTransferItem object is not in the read/write mode or the read-only mode, then return null.
        let Some((store, index)) = self
            .store_and_index()
            .filter(|(store, _)| store.mode() != Mode::Protected)
        else {
            return Ok(None);
        };

        // 2. If the drag data item kind is not File, then return null.
        let (is_file, _, _) = store.item(index);
        if !is_file {
            return Ok(None);
        }

        // 3. Return a new File object representing the actual data of the item represented by the DataTransferItem object.
        Ok(Some(store.create_file(cx.vm(), index)))
    }

    // https://wicg.github.io/entries-api/#dom-datatransferitem-webkitgetasentry
    pub(crate) fn webkit_get_as_entry(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // 1. Let store be this's DataTransfer object’s drag data store.

        // 2. If store’s drag data store mode is not read/write mode or read-only mode, return null and abort these steps
        let Some((store, index)) = self
            .store_and_index()
            .filter(|(store, _)| store.mode() != Mode::Protected)
        else {
            return Ok(None);
        };

        // 3. Let item be the item in store’s drag data store item list that this represents.
        let (is_file, _, _) = store.item(index);

        // 4. If item’s kind is not File, then return null and abort these steps.
        if !is_file {
            return Ok(None);
        }

        // 5. Return a new FileSystemEntry object representing the entry.
        Ok(Some(store.create_file_entry(index, cx.realm())))
    }
}
