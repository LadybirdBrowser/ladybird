/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! DataTransfer, https://html.spec.whatwg.org/multipage/dnd.html#the-datatransfer-interface

use core::cell::{self, Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Gc, GcRefCell, Must, Object, Realm, Trace, Utf16View, Value, Vm};

use super::data_transfer_item::DataTransferItem;
use super::data_transfer_item_list::DataTransferItemList;
use crate::bindings::conversions::{CppInterface, ToJs, frozen_array_to_js, sequence_to_js};
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi, cell_into_abi};
use crate::bindings::setlike::WorldEntries;
use crate::bindings::{CallContext, WebIdlResult};
use crate::file_api::file::File;
use crate::file_api::file_list::FileList;

unsafe extern "C" {
    fn web_data_transfer_create_writable_store() -> *mut c_void;
    fn web_data_transfer_store_unref(store: *mut c_void);
    fn web_data_transfer_store_mode(store: *mut c_void) -> u8;
    fn web_data_transfer_store_size(store: *mut c_void) -> usize;
    fn web_data_transfer_store_item(store: *mut c_void, index: usize) -> DragDataStoreItemView;
    fn web_data_transfer_store_add_text(store: *mut c_void, type_: usize, data: usize);
    fn web_data_transfer_store_remove_item(store: *mut c_void, index: usize);
    fn web_data_transfer_store_add_file(store: *mut c_void, file: *mut c_void);
    fn web_data_transfer_create_file(store: *mut c_void, index: usize) -> *mut c_void;
    fn web_data_transfer_create_file_entry(store: *mut c_void, index: usize, realm: *mut c_void) -> *mut c_void;
}

pub(super) const READ_WRITE: u8 = 0;
pub(super) const PROTECTED: u8 = 2;

// The store is shared with the native drag, drop and clipboard handlers. This holds one reference to it.
pub(super) struct DragDataStore(NonNull<c_void>);

impl Drop for DragDataStore {
    fn drop(&mut self) {
        // SAFETY: This owns a reference to the store.
        unsafe { web_data_transfer_store_unref(self.0.as_ptr()) };
    }
}

#[repr(C)]
struct DragDataStoreItemView {
    type_string: usize,
    data: usize,
    is_file: bool,
}

impl DragDataStore {
    pub(super) fn mode(&self) -> u8 {
        // SAFETY: This holds the store alive.
        unsafe { web_data_transfer_store_mode(self.0.as_ptr()) }
    }

    pub(super) fn len(&self) -> usize {
        // SAFETY: This holds the store alive.
        unsafe { web_data_transfer_store_size(self.0.as_ptr()) }
    }

    pub(super) fn item(&self, index: usize) -> (bool, Utf16FlyString, Utf16String) {
        // SAFETY: All callers pass an index within the live store. The strings are lent by that item.
        unsafe {
            let item = web_data_transfer_store_item(self.0.as_ptr(), index);
            (
                item.is_file,
                Utf16FlyString::from_raw_borrowed(item.type_string),
                Utf16String::from_raw_borrowed(item.data),
            )
        }
    }

    pub(super) fn add_text(&self, type_string: &Utf16FlyString, data: &Utf16String) {
        // SAFETY: This holds the store alive and the strings are lent for the call.
        unsafe { web_data_transfer_store_add_text(self.0.as_ptr(), type_string.raw_identity(), data.raw_identity()) };
    }

    pub(super) fn add_file(&self, file: Ref<File>) {
        // SAFETY: This holds the store alive and the file is live.
        unsafe { web_data_transfer_store_add_file(self.0.as_ptr(), cell_into_abi(file)) };
    }

    pub(super) fn create_file(&self, index: usize) -> Ref<File> {
        // SAFETY: This holds the store alive, and the index refers to a file item. LibWeb returns the new File cell.
        unsafe { cell_from_abi(web_data_transfer_create_file(self.0.as_ptr(), index)) }
    }

    pub(super) fn create_file_entry(&self, index: usize, realm: Gc<Realm>) -> CppInterface {
        // SAFETY: The store and realm are live, and the index refers to a file item.
        let wrapper = unsafe { web_data_transfer_create_file_entry(self.0.as_ptr(), index, realm.as_ptr().cast()) };
        let wrapper = NonNull::new(wrapper.cast()).expect("LibWeb wraps the file entry");
        // SAFETY: LibWeb returns a live FileSystemEntry wrapper.
        unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }

    fn remove_item(&self, index: usize) {
        // SAFETY: The transfer's item list and the store have matching indices.
        unsafe { web_data_transfer_store_remove_item(self.0.as_ptr(), index) };
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DataTransfer {
    #[gc(untraced)]
    store: RefCell<Option<DragDataStore>>,
    #[gc(untraced)]
    drop_effect: RefCell<Utf16FlyString>,
    #[gc(untraced)]
    effect_allowed: RefCell<Utf16FlyString>,
    items: Cell<Option<Ref<DataTransferItemList>>>,
    item_list: GcRefCell<Vec<Ref<DataTransferItem>>>,
    #[gc(untraced)]
    types: RefCell<Vec<Utf16String>>,
    types_arrays: WorldEntries<Object>,
}

impl Wrappable for DataTransfer {}

impl DataTransfer {
    fn create(vm: &Vm, store: DragDataStore) -> Ref<Self> {
        let count = store.len();
        let transfer = allocate(vm, |_| Self {
            store: RefCell::new(Some(store)),
            drop_effect: RefCell::new(Utf16FlyString::from_utf8("none")),
            effect_allowed: RefCell::new(Utf16FlyString::from_utf8("none")),
            items: Cell::default(),
            item_list: GcRefCell::new(Vec::with_capacity(count)),
            types: RefCell::default(),
            types_arrays: WorldEntries::default(),
        });

        for index in 0..count {
            Self::append_item(&transfer, vm, index);
        }
        transfer.update_types();
        transfer
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        // 1. Set the drag data store's item list to be an empty list.
        // 2. Set the drag data store's mode to read/write mode.
        // SAFETY: LibWeb transfers one reference to a new, empty read/write store.
        let store = unsafe { web_data_transfer_create_writable_store() };
        let store = DragDataStore(NonNull::new(store).expect("LibWeb creates a drag data store"));

        // 3. Set the dropEffect and effectAllowed to "none".
        // NB: create() sets both to "none".
        Ok(Self::create(cx.vm(), store))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-dropeffect
    pub(crate) fn drop_effect(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(Utf16String::from(&*self.drop_effect.borrow()))
    }

    pub(crate) fn set_drop_effect(&self, _: &mut CallContext, effect: Utf16String) -> WebIdlResult<()> {
        self.set_drop_effect_value(Utf16FlyString::from_utf16_string(&effect));
        Ok(())
    }

    fn set_drop_effect_value(&self, effect: Utf16FlyString) {
        // On setting, if the new value is one of "none", "copy", "link", or "move", then the attribute's current value
        // must be set to the new value. Other values must be ignored.
        let value = Utf16String::from(&effect);
        if ["none", "copy", "link", "move"].iter().any(|valid| value == *valid) {
            *self.drop_effect.borrow_mut() = effect;
        }
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-effectallowed
    pub(crate) fn effect_allowed(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(Utf16String::from(&*self.effect_allowed.borrow()))
    }

    pub(crate) fn set_effect_allowed(&self, _: &mut CallContext, effect: Utf16String) -> WebIdlResult<()> {
        // On setting, if the drag data store's mode is the read/write mode and the new value is one of "none", "copy",
        // "copyLink", "copyMove", "link", "linkMove", "move", "all", or "uninitialized", then the attribute's current
        // value must be set to the new value. Otherwise, it must be left unchanged.
        if self
            .store
            .borrow()
            .as_ref()
            .is_some_and(|store| store.mode() == READ_WRITE)
        {
            self.set_effect_allowed_internal(Utf16FlyString::from_utf16_string(&effect));
        }
        Ok(())
    }

    fn set_effect_allowed_internal(&self, effect: Utf16FlyString) {
        // AD-HOC: We need to be able to set the effectAllowed attribute internally regardless of the state of the drag
        //         data store.
        let value = Utf16String::from(&effect);
        if [
            "none",
            "copy",
            "copyLink",
            "copyMove",
            "link",
            "linkMove",
            "move",
            "all",
            "uninitialized",
        ]
        .iter()
        .any(|valid| value == *valid)
        {
            *self.effect_allowed.borrow_mut() = effect;
        }
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-items
    pub(crate) fn items(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<Ref<DataTransferItemList>> {
        // The items attribute must return a DataTransferItemList object associated with the DataTransfer object.
        Ok(this.items.get().unwrap_or_else(|| {
            let items = DataTransferItemList::create(cx.vm(), *this);
            this.items.set(Some(items));
            items
        }))
    }

    pub(super) fn store(&self) -> Option<cell::Ref<'_, DragDataStore>> {
        cell::Ref::filter_map(self.store.borrow(), Option::as_ref).ok()
    }

    pub(super) fn item(&self, index: usize) -> Ref<DataTransferItem> {
        self.item_list.borrow()[index]
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-types
    pub(crate) fn types(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The types attribute must return this DataTransfer object's types array.
        let array = self.types_arrays.get_or_create(cx, || {
            let types = self.types.borrow().clone();
            let array = sequence_to_js(cx, types, |r#type| r#type.to_js(cx));
            frozen_array_to_js(cx, array).must().as_object()
        });
        Ok(Value::from_object(array))
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-getdata
    pub(crate) fn get_data(&self, _: &mut CallContext, format: Utf16String) -> WebIdlResult<Utf16String> {
        // 1. If the DataTransfer object is no longer associated with a drag data store, then return the empty string.
        let store = self.store.borrow();
        let Some(store) = store.as_ref() else {
            return Ok(Utf16String::default());
        };

        // 2. If the drag data store's mode is the protected mode, then return the empty string.
        if store.mode() == PROTECTED {
            return Ok(Utf16String::default());
        }

        // 3. Let format be the first argument, converted to ASCII lowercase.
        // 4. Let convert-to-URL be false.
        // 5. If format equals "text", change it to "text/plain".
        // 6. If format equals "url", change it to "text/uri-list" and set convert-to-URL to true.
        let format = normalize_format(&format);

        // 7. If there is no item in the drag data store item list whose kind is text and whose type string is equal to
        //    format, return the empty string.
        let index = (0..store.len()).find(|&index| {
            let (is_file, type_string, _) = store.item(index);
            !is_file && Utf16View::of_fly_string(&type_string).equals_ignoring_ascii_case(format)
        });
        let Some(index) = index else {
            return Ok(Utf16String::default());
        };

        // 8. Let result be the data of the item in the drag data store item list whose kind is Plain Unicode string and
        //    whose type string is equal to format.
        let (_, _, result) = store.item(index);

        // FIXME: 9. If convert-to-URL is true, then parse result as appropriate for text/uri-list data, and then set
        //           result to the first URL from the list, if any, or the empty string otherwise.

        // 10. Return result.
        Ok(result)
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-setdata
    pub(crate) fn set_data(
        this: &Ref<Self>,
        cx: &mut CallContext,
        format: Utf16String,
        data: Utf16String,
    ) -> WebIdlResult<()> {
        // 1. If the DataTransfer object is no longer associated with a drag data store, return. Nothing happens.
        let store = this.store.borrow();
        let Some(store) = store.as_ref() else {
            return Ok(());
        };

        // 2. If the drag data store's mode is not the read/write mode, return. Nothing happens.
        if store.mode() != READ_WRITE {
            return Ok(());
        }

        // 3. Let format be the first argument, converted to ASCII lowercase.
        let format = format.to_ascii_lowercase();

        // 4. If format equals "text", change it to "text/plain". If format equals "url", change it to "text/uri-list".
        let format = normalize_format(&format).to_utf16_fly_string();

        // 5. Remove the item in the drag data store item list whose kind is text and whose type string is equal to
        //    format, if there is one.
        let previous_item = (0..store.len()).find(|&index| {
            let (is_file, type_string, _) = store.item(index);
            !is_file && type_string == format
        });
        if let Some(index) = previous_item {
            this.remove_item(cx.vm(), index);
        }

        // 6. Add an item to the drag data store item list whose kind is text, whose type string is equal to format, and
        //    whose data is the string given by the method's second argument.
        store.add_text(&format, &data);
        Self::did_add_item(this, cx.vm());
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-cleardata
    pub(crate) fn clear_data(&self, cx: &mut CallContext, format: Option<Utf16String>) -> WebIdlResult<()> {
        // 1. If the DataTransfer object is no longer associated with a drag data store, return. Nothing happens.
        let store = self.store.borrow();
        let Some(store) = store.as_ref() else {
            return Ok(());
        };

        // 2. If the drag data store's mode is not the read/write mode, return. Nothing happens.
        if store.mode() != READ_WRITE {
            return Ok(());
        }

        // 3. If the method was called with no arguments, remove each item in the drag data store item list whose kind is
        //    Plain Unicode string, and return.
        let Some(format) = format else {
            self.remove_text_items(cx.vm(), store, |_| true);
            return Ok(());
        };

        // 4. Set format to format, converted to ASCII lowercase.
        // 5. If format equals "text", change it to "text/plain". If format equals "url", change it to "text/uri-list".
        let format = normalize_format(&format);

        // 6. Remove the item in the drag data store item list whose kind is text and whose type string is equal to
        //    format, if there is one.
        self.remove_text_items(cx.vm(), store, |type_string| {
            Utf16View::of_fly_string(type_string).equals_ignoring_ascii_case(format)
        });
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-files
    pub(crate) fn files(&self, cx: &mut CallContext) -> WebIdlResult<Ref<FileList>> {
        let vm = cx.vm();

        // 1. Start with an empty list L.
        let files = allocate(vm, |defer_gc| FileList::new(defer_gc, &[]));

        // 2. If the DataTransfer object is no longer associated with a drag data store, the FileList is empty. Return
        //    the empty list L.
        let store = self.store.borrow();
        let Some(store) = store.as_ref() else {
            return Ok(files);
        };

        // 3. If the drag data store's mode is the protected mode, return the empty list L.
        if store.mode() == PROTECTED {
            return Ok(files);
        }

        // 4. For each item in the drag data store item list whose kind is File, add the item's data (the file, in
        //    particular its name and contents, as well as its type) to the list L.
        for index in 0..store.len() {
            let (is_file, _, _) = store.item(index);
            if is_file {
                files.add_file(vm, store.create_file(index));
            }
        }

        // 5. The files found by these steps are those in the list L.
        Ok(files)
    }

    fn append_item(this: &Ref<Self>, vm: &Vm, index: usize) -> Ref<DataTransferItem> {
        let item = DataTransferItem::create(vm, *this, index);
        this.item_list.borrow_mut(vm).push(item);
        item
    }

    pub(super) fn did_add_item(this: &Ref<Self>, vm: &Vm) -> Ref<DataTransferItem> {
        let index = this.item_list.borrow().len();
        let item = Self::append_item(this, vm, index);
        this.update_types();
        item
    }

    pub(super) fn remove_item(&self, vm: &Vm, index: usize) {
        let mut items = self.item_list.borrow_mut(vm);
        let store = self.store().expect("removing an item requires a drag data store");
        store.remove_item(index);
        items.remove(index).set_index(None);
        for (index, item) in items.iter().enumerate().skip(index) {
            item.set_index(Some(index));
        }
        drop(items);
        self.update_types();
    }

    fn remove_text_items(&self, vm: &Vm, store: &DragDataStore, matches: impl Fn(&Utf16FlyString) -> bool) {
        for index in (0..store.len()).rev() {
            let (is_file, type_string, _) = store.item(index);
            if !is_file && matches(&type_string) {
                self.remove_item(vm, index);
            }
        }
    }

    // https://html.spec.whatwg.org/multipage/dnd.html#concept-datatransfer-types
    fn update_types(&self) {
        // 1. Let L be an empty sequence.
        let mut types = Vec::new();

        // 2. If the DataTransfer object is still associated with a drag data store:
        if let Some(store) = self.store.borrow().as_ref() {
            let mut contains_file = false;

            // 1. For each item in the DataTransfer object's drag data store item list whose kind is text, add an entry
            //    to L consisting of the item's type string.
            for index in 0..store.len() {
                let (is_file, type_string, _) = store.item(index);
                if is_file {
                    contains_file = true;
                } else {
                    types.push(Utf16String::from(type_string));
                }
            }

            // 2. If there are any items in the DataTransfer object's drag data store item list whose kind is File, then
            //    add an entry to L consisting of the string "Files". (This value can be distinguished from the other
            //    values because it is not lowercase.)
            if contains_file {
                types.push(Utf16String::from_ascii(b"Files"));
            }
        }

        // 3. Set the DataTransfer object's types array to the result of creating a frozen array from L.
        *self.types.borrow_mut() = types;
        self.types_arrays.clear();
    }
}

fn normalize_format(format: &Utf16String) -> Utf16View<'_> {
    let format = Utf16View::of_string(format);
    if format.equals_ignoring_ascii_case(Utf16View::Ascii(b"text")) {
        Utf16View::Ascii(b"text/plain")
    } else if format.equals_ignoring_ascii_case(Utf16View::Ascii(b"url")) {
        Utf16View::Ascii(b"text/uri-list")
    } else {
        format
    }
}

/// # Safety
///
/// The caller transfers one ownership reference to a live drag data store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_data_transfer_create(vm: &Vm, store: NonNull<c_void>) -> *mut c_void {
    cell_into_abi(DataTransfer::create(vm, DragDataStore(store)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_data_transfer_disassociate(transfer: *mut c_void) {
    // SAFETY: LibWeb passes a live DataTransfer.
    let transfer = unsafe { cell_from_abi::<DataTransfer>(transfer) };
    transfer.store.borrow_mut().take();
    transfer.update_types();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_data_transfer_drop_effect(transfer: *mut c_void) -> usize {
    // SAFETY: LibWeb passes a live DataTransfer.
    unsafe { cell_from_abi::<DataTransfer>(transfer) }
        .drop_effect
        .borrow()
        .clone()
        .into_raw()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_data_transfer_effect_allowed(transfer: *mut c_void) -> usize {
    // SAFETY: LibWeb passes a live DataTransfer.
    unsafe { cell_from_abi::<DataTransfer>(transfer) }
        .effect_allowed
        .borrow()
        .clone()
        .into_raw()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_data_transfer_set_drop_effect(transfer: *mut c_void, effect: usize) {
    // SAFETY: LibWeb passes a live DataTransfer and lends a fly string.
    unsafe {
        cell_from_abi::<DataTransfer>(transfer).set_drop_effect_value(Utf16FlyString::from_raw_borrowed(effect));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_data_transfer_set_effect_allowed_internal(transfer: *mut c_void, effect: usize) {
    // SAFETY: LibWeb passes a live DataTransfer and lends a fly string.
    unsafe {
        cell_from_abi::<DataTransfer>(transfer).set_effect_allowed_internal(Utf16FlyString::from_raw_borrowed(effect));
    }
}
