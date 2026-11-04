/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage, https://html.spec.whatwg.org/multipage/webstorage.html#storage-2

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{EmbedderClass, EmbedderValue, Gc, MarkedVec, Realm, Throw, Trace, Value, Visitor, Vm};
use liburl_rust::url::ExcludeFragment;

use crate::bindings::conversions::UsvString;
use crate::bindings::environment_settings_object::{CppEnvironmentSettingsObject, relevant_settings_object};
use crate::bindings::global_object::{CppWindow, active_windows, relevant_global_object, window_from_global_object};
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::event::EventInit;
use crate::html::storage_event::{StorageEvent, StorageEventInit};
use crate::storage_api::storage_bottle::StorageBottle;
use crate::storage_api::storage_key::{StorageKey, obtain_a_storage_key};
use crate::webidl::dom_exception::DOMExceptionFields;
use crate::webidl::quota_exceeded_error::{self, QuotaExceededErrorFields};

unsafe extern "C" {
    fn web_html_window_storage(window: *mut c_void, storage_type: u8) -> *mut c_void;
}

/// https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-type
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum StorageType {
    Local = 0,
    Session = 1,
}

impl StorageType {
    /// The storage type that LibWeb passes, an HTML::Storage::Type.
    pub(crate) fn from_abi(value: u8) -> Self {
        match value {
            0 => StorageType::Local,
            1 => StorageType::Session,
            _ => panic!("LibWeb passes a storage type"),
        }
    }
}

// SAFETY: A storage type holds no cells.
unsafe impl Trace for StorageType {
    fn trace(&self, _: &mut Visitor) {}
}

// https://html.spec.whatwg.org/multipage/webstorage.html#storage-2
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Storage {
    realm: Gc<Realm>,
    storage_type: StorageType,
    map: StorageBottle,
}

impl Wrappable for Storage {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

/// The key, values and URL of a change to a storage area, which the storage events for it carry.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct StorageChange {
    key: Option<Utf16String>,
    old_value: Option<Utf16String>,
    new_value: Option<Utf16String>,
    #[gc(untraced)]
    url: UsvString,
}

static STORAGE_CHANGE_CLASS: EmbedderClass<StorageChange> = EmbedderClass::new("StorageChange");

impl EmbedderValue for StorageChange {
    const CLASS: &'static EmbedderClass<Self> = &STORAGE_CHANGE_CLASS;
}

/// The Storage object that the Window's localStorage or sessionStorage getter returns, if it does not throw.
fn window_storage(window: &CppWindow, storage_type: StorageType) -> Option<Ref<Storage>> {
    // SAFETY: The Window is live, and LibWeb returns null or the live cell of a Storage object.
    let storage = unsafe { web_html_window_storage(window.as_ptr(), storage_type as u8) };
    // SAFETY: A non-null pointer is the live cell of a Storage object.
    (!storage.is_null()).then(|| unsafe { cell_from_abi::<Storage>(storage) })
}

/// Fires the storage events for a change to a storage area of the type with the storage key, made by the script of
/// excluded_storage when the change was made in this process: steps 3 and 4 of broadcast.
///
/// NB: The map lives in the browser process and is mirrored here; the mirror tells the browser of a change made here,
///     and the browser relays it to every other process mirroring the map, where these steps run with no
///     excluded_storage. session_page is the page of the change's traversable navigable, for a "session" type.
#[allow(clippy::too_many_arguments, reason = "a change has these parts")]
pub(crate) fn dispatch_storage_events(
    vm: &Vm,
    storage_type: StorageType,
    storage_key: &StorageKey,
    session_page: Option<u64>,
    key: Option<Utf16String>,
    old_value: Option<Utf16String>,
    new_value: Option<Utf16String>,
    url: &str,
    excluded_storage: Option<Ref<Storage>>,
) {
    // 3. Let remoteStorages be all Storage objects excluding storage whose:
    let remote_storages = MarkedVec::new(vm);

    // AD-HOC: The specification defines this by iterating over created Storage objects. However, Storage objects are
    //         created lazily when accessed through window.localStorage or window.sessionStorage. This means that events
    //         will not be fired to Windows who have not had these properties accessed - which is not expected.
    //         See: https://github.com/whatwg/html/issues/10135. To solve this, we instead iterate over all active
    //         windows, and initialize any storage objects as part of this loop.
    let windows = active_windows(vm);
    for window in windows.values().iter() {
        // * type is storage's type
        let Some(storage) = window_storage(window, storage_type) else {
            continue;
        };

        // "excluding storage"
        if Some(storage) == excluded_storage {
            continue;
        }

        // * relevant settings object's origin is same origin with storage's relevant settings object's origin
        // NB: A storage key is an origin, and two keys are equal when their origins are same origin. Compared as the
        //     keys the bottles are kept under, so the file: documents that share one bottle hear each other.
        if obtain_a_storage_key(&storage.relevant_settings_object()).as_ref() != Some(storage_key) {
            continue;
        }

        // * and, if type is "session", whose relevant settings object's associated Document's node navigable's
        //   traversable navigable is thisDocument's node navigable's traversable navigable.
        // NB: A page's documents all belong to one traversable, so the page stands for the traversable here.
        if storage_type == StorageType::Session {
            // NB: It is possible the remote storage may have not been fully teared down immediately at the point it's
            //     document is made inactive.
            if !window.associated_document().has_navigable() {
                continue;
            }

            if session_page != Some(window.page_id()) {
                continue;
            }
        }

        remote_storages.push(storage);
    }

    if remote_storages.is_empty() {
        return;
    }

    let change = STORAGE_CHANGE_CLASS.allocate(vm, |_| StorageChange {
        key,
        old_value,
        new_value,
        url: UsvString::from_utf8(url),
    });

    // 4. For each remoteStorage of remoteStorages: queue a global task on the DOM manipulation task source given
    //    remoteStorage's relevant global object to fire an event named storage at remoteStorage's relevant global
    //    object, using StorageEvent, with key initialized to key, oldValue initialized to oldValue, newValue
    //    initialized to newValue, url initialized to url, and storageArea initialized to remoteStorage.
    for &remote_storage in remote_storages.values().iter() {
        let global: &Storage = &remote_storage;
        queue_a_global_task(
            vm,
            relevant_global_object(global),
            TaskSource::DOMManipulation,
            (remote_storage, change),
            |vm, (remote_storage, change)| {
                let event_init = StorageEventInit {
                    parent: EventInit {
                        bubbles: false,
                        cancelable: false,
                        composed: false,
                    },
                    key: change.key.clone(),
                    old_value: change.old_value.clone(),
                    new_value: change.new_value.clone(),
                    url: change.url.clone(),
                    storage_area: Some(remote_storage),
                };
                let event =
                    StorageEvent::create(vm, remote_storage.realm, &Utf16String::from_utf8("storage"), event_init);
                event.event().set_is_trusted(true);
                remote_storage.window().as_event_target().dispatch_event(event.event());
            },
        );
    }
}

impl Storage {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, storage_type: StorageType, map: StorageBottle) -> Ref<Storage> {
        allocate(vm, |_| Storage {
            realm,
            storage_type,
            map,
        })
    }

    pub(crate) fn map(&self) -> &StorageBottle {
        &self.map
    }

    /// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-settings-object
    fn relevant_settings_object(&self) -> CppEnvironmentSettingsObject {
        relevant_settings_object(relevant_global_object(self))
    }

    /// The Window that is the relevant global object.
    fn window(&self) -> CppWindow {
        window_from_global_object(relevant_global_object(self)).expect("a Storage belongs to a Window")
    }

    /// The serialization of the URL of the associated Document of the relevant global object.
    fn document_url(&self) -> String {
        self.window()
            .associated_document()
            .url()
            .serialize(ExcludeFragment::No)
            .to_owned()
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The length getter steps are to return this's map's size.
        Ok(u32::try_from(self.map.size()).expect("the quota keeps a map below 2^32 entries"))
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-key
    pub(crate) fn key(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Utf16String>> {
        // 1. If index is greater than or equal to this's map's size, then return null.
        // 2. Let keys be the result of running get the keys on this's map.
        // NB: We combine these steps so we don't have to do two IPC calls and cause a race condition if the storage bottle
        //     changes size between them.
        let keys = self.map.keys();

        // 3. Return keys[index].
        Ok(keys.into_iter().nth(index as usize))
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-getitem
    pub(crate) fn get_item(&self, _: &mut CallContext, key: Utf16String) -> WebIdlResult<Option<Utf16String>> {
        // 1. If this's map[key] does not exist, then return null.
        // 2. Return this's map[key].
        Ok(self.map.get(&key))
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-setitem
    pub(crate) fn set_item(
        this: &Ref<Self>,
        cx: &mut CallContext,
        key: Utf16String,
        value: Utf16String,
    ) -> WebIdlResult<()> {
        // 1. Let oldValue be null.
        // 2. Let reorder be true.
        let mut reorder = true;

        // 3. If this's map[key] exists:
        //     1. Set oldValue to this's map[key].
        //     2. If oldValue is value, then return.
        //     3. Set reorder to false.
        // 4. If value cannot be stored, then throw a "QuotaExceededError" DOMException.
        // 5. Set this's map[key] to value.

        let url = this.document_url();
        let Ok(old_value) = this.map.set(&key, &value, &url) else {
            return Err(this.quota_exceeded_error(cx));
        };

        if let Some(old_value) = &old_value {
            if *old_value == value {
                return Ok(());
            }

            reorder = false;
        }

        // 6. If reorder is true, then reorder this.
        if reorder {
            this.reorder();
        }

        // 7. Broadcast this with key, oldValue, and value.
        Self::broadcast(this, cx.vm(), Some(key), old_value, Some(value), &url);

        Ok(())
    }

    fn quota_exceeded_error(&self, cx: &CallContext) -> Exception {
        let quota = self.map.quota().expect("a bottle that refuses a value has a quota");
        let fields = QuotaExceededErrorFields {
            dom_exception: DOMExceptionFields {
                name: Utf16String::from_utf8("QuotaExceededError"),
                message: Utf16String::from_utf8(&format!("Unable to store more than {quota} bytes in storage")),
            },
            quota: None,
            requested: None,
        };
        Exception::Thrown(Throw::new(Value::from_object(quota_exceeded_error::create(
            cx.realm(),
            &fields,
        ))))
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-removeitem
    pub(crate) fn remove_item(this: &Ref<Self>, cx: &mut CallContext, key: Utf16String) -> WebIdlResult<()> {
        // 1. If this's map[key] does not exist, then return.
        // 2. Set oldValue to this's map[key].
        let Some(old_value) = this.map.get(&key) else {
            return Ok(());
        };

        // 3. Remove this's map[key].
        let url = this.document_url();
        this.map.remove(&key, &url);

        // 4. Reorder this.
        this.reorder();

        // 5. Broadcast this with key, oldValue, and null.
        Self::broadcast(this, cx.vm(), Some(key), Some(old_value), None, &url);

        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#dom-storage-clear
    pub(crate) fn clear(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. If this's map is empty, then return.
        if this.map.size() == 0 {
            return Ok(());
        }

        // 2. Clear this's map.
        let url = this.document_url();
        this.map.clear(&url);

        // 3. Broadcast this with null, null, and null.
        Self::broadcast(this, cx.vm(), None, None, None, &url);

        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-reorder
    #[allow(
        clippy::unused_self,
        reason = "reordering is implementation-defined, and does nothing here"
    )]
    fn reorder(&self) {
        // To reorder a Storage object storage, reorder storage's map's entries in an implementation-defined manner.
        // NOTE: This basically means that we're not required to maintain any particular iteration order.
    }

    // https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-broadcast
    fn broadcast(
        this: &Ref<Self>,
        vm: &Vm,
        key: Option<Utf16String>,
        old_value: Option<Utf16String>,
        new_value: Option<Utf16String>,
        url: &str,
    ) {
        // 1. Let thisDocument be storage's relevant global object's associated Document.
        // 2. Let url be the serialization of thisDocument's URL.
        // NB: The caller serialized it, for the bottle; it is the same document's URL.

        // NB: The key the bottle is kept under, not the bare origin: Every file: document shares one bottle (see "obtain
        //     a storage key"), so each hears the others' changes. A window that has no key has no bottle to have written.
        let Some(storage_key) = obtain_a_storage_key(&this.relevant_settings_object()) else {
            return;
        };
        let session_page = (this.storage_type == StorageType::Session).then(|| this.window().page_id());

        // Steps 3 and 4.
        dispatch_storage_events(
            vm,
            this.storage_type,
            &storage_key,
            session_page,
            key,
            old_value,
            new_value,
            url,
            Some(*this),
        );
    }

    pub(crate) fn supported_property_names(&self, _: &mut CallContext) -> Vec<Utf16String> {
        // The supported property names on a Storage object storage are the result of running get the keys on storage's
        // map.
        self.map.keys()
    }
}
