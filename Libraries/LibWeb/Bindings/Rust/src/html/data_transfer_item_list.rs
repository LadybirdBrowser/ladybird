/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! DataTransferItemList, https://html.spec.whatwg.org/multipage/dnd.html#the-datatransferitemlist-interface

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Trace, Utf16StringBuilder, Utf16View, Vm};

use super::data_transfer::{DataTransfer, READ_WRITE};
use super::data_transfer_item::DataTransferItem;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::file_api::file::File;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DataTransferItemList {
    data_transfer: Ref<DataTransfer>,
}

impl Wrappable for DataTransferItemList {}

impl DataTransferItemList {
    pub(super) fn create(vm: &Vm, data_transfer: Ref<DataTransfer>) -> Ref<Self> {
        allocate(vm, |_| Self { data_transfer })
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The length attribute must return zero if the object is in the disabled mode; otherwise it must return the number
        // of items in the drag data store item list.
        let length = self.data_transfer.store().map_or(0, |store| store.len());
        Ok(u32::try_from(length).expect("a transfer has fewer than 2^32 items"))
    }

    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Ref<DataTransferItem>> {
        // The generated indexed property getter checks the index against length.
        Ok(self.data_transfer.item(index as usize))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-add
    pub(crate) fn add_string(
        &self,
        cx: &mut CallContext,
        data: Utf16String,
        type_string: Utf16String,
    ) -> WebIdlResult<Option<Ref<DataTransferItem>>> {
        // 1. If the DataTransferItemList object is not in the read/write mode, return null.
        let Some(store) = self.data_transfer.store().filter(|store| store.mode() == READ_WRITE) else {
            return Ok(None);
        };

        // 2. Jump to the appropriate set of steps from the following list:
        //    -> If the first argument to the method is a string

        // If there is already an item in the drag data store item list whose kind is text and whose type string is equal
        // to the value of the method's second argument, converted to ASCII lowercase, then throw a "NotSupportedError"
        // DOMException.
        let format = Utf16View::of_string(&type_string);
        if (0..store.len()).any(|index| {
            let (is_file, item_type, _) = store.item(index);
            !is_file && Utf16View::of_fly_string(&item_type).equals_ignoring_ascii_case(format)
        }) {
            let mut message = Utf16StringBuilder::new();
            message.append_ascii("There is already a DataTransferItem with type ");
            message.append(format);
            return Err(Exception::Dom("NotSupportedError", message.to_utf16_string()));
        }

        // Otherwise, add an item to the drag data store item list whose kind is text, whose type string is equal to the
        // value of the method's second argument, converted to ASCII lowercase, and whose data is the string given by the
        // method's first argument.
        let lowercase_type = Utf16FlyString::from_utf16_string(&type_string.to_ascii_lowercase());
        store.add_text(&lowercase_type, &data);

        // 3. Determine the value of the indexed property corresponding to the newly added item, and return that value (a
        //    newly created DataTransferItem object).
        Ok(Some(DataTransfer::did_add_item(&self.data_transfer, cx.vm())))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-add
    pub(crate) fn add_file(
        &self,
        cx: &mut CallContext,
        file: Ref<File>,
    ) -> WebIdlResult<Option<Ref<DataTransferItem>>> {
        // 1. If the DataTransferItemList object is not in the read/write mode, return null.
        let Some(store) = self.data_transfer.store().filter(|store| store.mode() == READ_WRITE) else {
            return Ok(None);
        };

        // 2. Jump to the appropriate set of steps from the following list:
        //     -> If the first argument to the method is a File

        // Add an item to the drag data store item list whose kind is File, whose type string is the type of the File,
        // converted to ASCII lowercase, and whose data is the same as the File's data.
        store.add_file(file);

        // 3. Determine the value of the indexed property corresponding to the newly added item, and return that value (a
        //    newly created DataTransferItem object).
        Ok(Some(DataTransfer::did_add_item(&self.data_transfer, cx.vm())))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-remove
    pub(crate) fn remove(&self, cx: &mut CallContext, index: u32) -> WebIdlResult<()> {
        // 1. If the DataTransferItemList object is not in the read/write mode, throw an "InvalidStateError" DOMException.
        let Some(store) = self.data_transfer.store().filter(|store| store.mode() == READ_WRITE) else {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"DataTransferItemList is not in read/write mode"),
            ));
        };

        // 2. If the drag data store does not contain an indexth item, then return.
        let index = index as usize;
        if index >= store.len() {
            return Ok(());
        }

        // 3. Remove the indexth item from the drag data store.
        self.data_transfer.remove_item(cx.vm(), index);
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-clear
    pub(crate) fn clear(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        // The clear() method, if the DataTransferItemList object is in the read/write mode, must remove all the items from
        // the drag data store. Otherwise, it must do nothing.
        let Some(store) = self.data_transfer.store().filter(|store| store.mode() == READ_WRITE) else {
            return Ok(());
        };

        for index in (0..store.len()).rev() {
            self.data_transfer.remove_item(cx.vm(), index);
        }
        Ok(())
    }
}
