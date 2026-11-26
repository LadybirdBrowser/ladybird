/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Drag data store, https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr::NonNull;
use std::rc::Rc;

use ak::{Span, Utf16FlyString, Utf16String};
use libjs_rust::api::{Gc, Realm, Utf16View, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::lent_string::{LentUtf16FlyString, LentUtf16String};
use crate::file_api::blob::{BlobPropertyBag, EndingType};
use crate::file_api::file::{File, FilePropertyBag};

unsafe extern "C" {
    #[allow(improper_ctypes, reason = "Gc passes a pointer to the realm cell")]
    fn web_drag_data_store_create_file_entry(file_name: Utf16String, realm: Gc<Realm>) -> NonNull<c_void>;
    fn web_drag_data_store_delete_byte_buffer(owner: NonNull<NativeByteBuffer>);
    fn web_drag_data_store_encode_file_data(bytes: Span<'_, u8>, result: &mut Base64Result);
}

#[repr(C)]
struct NativeByteBuffer {
    _opaque: [u8; 0],
}

#[repr(C)]
pub(super) struct Base64Result {
    _opaque: [u8; 0],
}

#[repr(C)]
pub(super) struct NativeFileBytes {
    owner: NonNull<NativeByteBuffer>,
    data: *const u8,
    length: usize,
}

impl NativeFileBytes {
    fn as_slice(&self) -> &[u8] {
        // SAFETY: The native owner keeps the bytes alive and unchanged until it is deleted.
        unsafe { ak::slice_from_raw_parts(self.data, self.length) }
    }
}

impl Drop for NativeFileBytes {
    fn drop(&mut self) {
        // SAFETY: C++ transfers ownership of the buffer to this value.
        unsafe { web_drag_data_store_delete_byte_buffer(self.owner) };
    }
}

enum FileBytes {
    Rust(Vec<u8>),
    Native(NativeFileBytes),
}

impl FileBytes {
    fn as_slice(&self) -> &[u8] {
        match self {
            Self::Rust(data) => data,
            Self::Native(data) => data.as_slice(),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(super) enum Mode {
    ReadWrite,
    #[allow(dead_code, reason = "Native drag and paste events set this mode through the C++ ABI")]
    ReadOnly,
    Protected,
}

// https://html.spec.whatwg.org/multipage/dnd.html#the-drag-data-item-kind
enum DragDataStoreItemData {
    Text(Utf16String),
    File { data: FileBytes, name: Utf16String },
}

struct DragDataStoreItem {
    // https://html.spec.whatwg.org/multipage/dnd.html#the-drag-data-item-type-string
    type_string: Utf16FlyString,
    data: DragDataStoreItemData,
}

impl DragDataStoreItem {
    fn is_file(&self) -> bool {
        matches!(self.data, DragDataStoreItemData::File { .. })
    }

    fn text(&self) -> Utf16String {
        match &self.data {
            DragDataStoreItemData::Text(data) => data.clone(),
            DragDataStoreItemData::File { .. } => Utf16String::default(),
        }
    }
}

/// https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store
pub(crate) struct DragDataStore {
    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-item-list
    item_list: RefCell<Vec<DragDataStoreItem>>,
    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-mode
    mode: Cell<Mode>,
    // https://html.spec.whatwg.org/multipage/dnd.html#drag-data-store-allowed-effects-state
    allowed_effects_state: RefCell<Utf16FlyString>,
}

impl DragDataStore {
    pub(super) fn create() -> Rc<Self> {
        Rc::new(Self {
            item_list: RefCell::default(),
            mode: Cell::new(Mode::Protected),
            allowed_effects_state: RefCell::new(Utf16FlyString::from_utf8("uninitialized")),
        })
    }

    pub(super) fn mode(&self) -> Mode {
        self.mode.get()
    }

    pub(super) fn set_mode(&self, mode: Mode) {
        self.mode.set(mode);
    }

    pub(super) fn allowed_effects_state(&self) -> Utf16FlyString {
        self.allowed_effects_state.borrow().clone()
    }

    pub(super) fn set_allowed_effects_state(&self, state: Utf16FlyString) {
        *self.allowed_effects_state.borrow_mut() = state;
    }

    pub(super) fn len(&self) -> usize {
        self.item_list.borrow().len()
    }

    pub(super) fn item(&self, index: usize) -> (bool, Utf16FlyString, Utf16String) {
        let items = self.item_list.borrow();
        let item = &items[index];
        (item.is_file(), item.type_string.clone(), item.text())
    }

    pub(super) fn add_text(&self, type_string: Utf16FlyString, data: Utf16String) {
        self.item_list.borrow_mut().push(DragDataStoreItem {
            type_string,
            data: DragDataStoreItemData::Text(data),
        });
    }

    fn add_file_data(&self, type_string: Utf16FlyString, data: FileBytes, name: Utf16String) {
        self.item_list.borrow_mut().push(DragDataStoreItem {
            type_string,
            data: DragDataStoreItemData::File { data, name },
        });
    }

    pub(super) fn add_file(&self, file: Ref<File>) {
        let blob = file.parent();
        let type_string = Utf16FlyString::from_utf16_string(&blob.type_string().to_ascii_lowercase());
        self.add_file_data(
            type_string,
            FileBytes::Rust(blob.raw_bytes().to_vec()),
            file.name_string().clone(),
        );
    }

    pub(super) fn remove_item(&self, index: usize) {
        self.item_list.borrow_mut().remove(index);
    }

    pub(super) fn create_file(&self, vm: &Vm, index: usize) -> Ref<File> {
        let file = {
            let items = self.item_list.borrow();
            let item = &items[index];
            let DragDataStoreItemData::File { data, name } = &item.data else {
                unreachable!("the item is a file");
            };
            let options = FilePropertyBag {
                parent: BlobPropertyBag {
                    endings: EndingType::Transparent,
                    r#type: Utf16String::from(&item.type_string),
                },
                // FIXME: Fill in other fields (e.g. last_modified).
                last_modified: None,
            };
            File::create(data.as_slice().to_vec(), name.clone(), &options)
        };
        allocate(vm, |_| file)
    }

    pub(super) fn create_file_entry(&self, index: usize, realm: Gc<Realm>) -> CppInterface {
        let name = {
            let items = self.item_list.borrow();
            let DragDataStoreItemData::File { name, .. } = &items[index].data else {
                unreachable!("the item is a file");
            };
            name.clone()
        };
        // SAFETY: The realm is live. LibWeb takes ownership of the name and returns a live FileSystemEntry wrapper.
        unsafe {
            let wrapper = web_drag_data_store_create_file_entry(name, realm);
            CppInterface::from_wrapper(Gc::from_non_null(wrapper.cast()))
        }
    }

    fn has_text_item(&self) -> bool {
        self.item_list
            .borrow()
            .iter()
            .any(|item| !item.is_file() && Utf16View::of_fly_string(&item.type_string) == "text/plain")
    }
}

#[repr(transparent)]
pub(super) struct OwnedDragDataStore(NonNull<DragDataStore>);

#[repr(transparent)]
pub struct LentDragDataStore<'a>(NonNull<DragDataStore>, PhantomData<&'a OwnedDragDataStore>);

impl LentDragDataStore<'_> {
    pub(crate) fn to_rc(&self) -> Rc<DragDataStore> {
        let store = self.0.as_ptr();
        // SAFETY: The C++ owner lends a live strong reference from Rc::into_raw.
        unsafe {
            Rc::increment_strong_count(store);
            Rc::from_raw(store)
        }
    }
}

impl Drop for OwnedDragDataStore {
    fn drop(&mut self) {
        // SAFETY: This handle owns the reference from Rc::into_raw.
        unsafe { Rc::decrement_strong_count(self.0.as_ptr()) };
    }
}

#[repr(C)]
pub(super) struct DragDataStoreItemView {
    type_string: usize,
    data: usize,
    is_file: bool,
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_create() -> OwnedDragDataStore {
    let store = Rc::into_raw(DragDataStore::create()).cast_mut();
    OwnedDragDataStore(NonNull::new(store).expect("Rc::into_raw produces a non-null pointer"))
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_unref(_: OwnedDragDataStore) {}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_set_mode(store: &DragDataStore, mode: Mode) {
    store.set_mode(mode);
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_size(store: &DragDataStore) -> usize {
    store.len()
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_has_text_item(store: &DragDataStore) -> bool {
    store.has_text_item()
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_item(store: &DragDataStore, index: usize) -> DragDataStoreItemView {
    let items = store.item_list.borrow();
    let item = &items[index];
    DragDataStoreItemView {
        type_string: item.type_string.clone().into_raw(),
        data: item.text().into_raw(),
        is_file: item.is_file(),
    }
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_file_data_as_base64(
    store: &DragDataStore,
    index: usize,
    result: &mut Base64Result,
) {
    let items = store.item_list.borrow();
    let bytes = match &items[index].data {
        DragDataStoreItemData::Text(_) => &[],
        DragDataStoreItemData::File { data, .. } => data.as_slice(),
    };
    // SAFETY: The encoder reads the bytes during this call and writes the result to the C++ caller.
    unsafe { web_drag_data_store_encode_file_data(Span::new(bytes), result) };
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_add_text(
    store: &DragDataStore,
    type_string: LentUtf16FlyString<'_>,
    data: LentUtf16String<'_>,
) {
    store.add_text(type_string.to_utf16_fly_string(), data.to_utf16_string());
}

#[unsafe(no_mangle)]
pub(super) extern "C" fn web_rust_drag_data_store_add_file(
    store: &DragDataStore,
    type_string: LentUtf16FlyString<'_>,
    data: NativeFileBytes,
    name: LentUtf16String<'_>,
) {
    store.add_file_data(
        type_string.to_utf16_fly_string(),
        FileBytes::Native(data),
        name.to_utf16_string(),
    );
}
