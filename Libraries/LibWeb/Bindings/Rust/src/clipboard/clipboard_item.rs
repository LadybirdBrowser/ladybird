/*
 * Copyright (c) 2024, Feng Yu <f3n67u@outlook.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ClipboardItem, https://w3c.github.io/clipboard-apis/#clipboard-item-interface

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, Realm, Trace, Value, Vm};
use libwebcommon_rust::mime_sniff::mime_type::MimeType;

use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context, wrap};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, deferred_invoke, promise_realm, queue_a_global_task, react_to_promise,
    reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::clipboard::clipboard::{
    SystemClipboardItem, clipboard_item_data_blob, os_specific_well_known_format, string_to_code_points,
};
use crate::file_api::blob::{AnyBlob, Blob};

pub(crate) const WEB_CUSTOM_FORMAT_PREFIX: &str = "web ";

/// The enumeration PresentationStyle of Clipboard/ClipboardItem.idl.
#[derive(Clone, Copy, Default, Eq, PartialEq)]
pub(crate) enum PresentationStyle {
    #[default]
    Unspecified,
    Inline,
    Attachment,
}

impl PresentationStyle {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Self::Unspecified => "unspecified",
            Self::Inline => "inline",
            Self::Attachment => "attachment",
        }
    }
}

/// The dictionary ClipboardItemOptions of Clipboard/ClipboardItem.idl.
pub(crate) struct ClipboardItemOptions {
    pub(crate) presentation_style: PresentationStyle,
}

/// The union (DOMString or Blob) of Clipboard/ClipboardItem.idl, whose members the bindings convert.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum DOMStringOrBlob {
    DOMString(Utf16String),
    Blob(AnyBlob),
}

// https://w3c.github.io/clipboard-apis/#representation
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Representation {
    pub(crate) mime_type: Utf16String,
    pub(crate) is_custom: bool,
    pub(crate) data: WebIdlPromise,
}

impl Representation {
    fn type_string(&self) -> Utf16String {
        if self.is_custom {
            return Utf16String::from_utf8(&format!("web {}", String::from_utf16_lossy(&self.mime_type.to_utf16())));
        }
        self.mime_type.clone()
    }
}

// https://w3c.github.io/clipboard-apis/#clipboard-item-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ClipboardItem {
    this: Cell<Option<Ref<ClipboardItem>>>,

    // https://w3c.github.io/clipboard-apis/#list-of-representations
    representations: GcRefCell<Vec<Representation>>,

    // https://w3c.github.io/clipboard-apis/#presentation-style
    #[gc(untraced)]
    pub(crate) presentation_style: Cell<PresentationStyle>,

    // https://w3c.github.io/clipboard-apis/#clipboarditem-types-array
    #[gc(untraced)]
    types: RefCell<Vec<Utf16String>>,

    // https://w3c.github.io/clipboard-apis/#clipboarditem-clipboard-change-count-at-read
    clipboard_change_count_at_read: Cell<Option<usize>>,

    // https://w3c.github.io/clipboard-apis/#clipboarditem-originating-system-clipboard-item
    #[gc(untraced)]
    originating_system_clipboard_item: RefCell<Option<SystemClipboardItem>>,

    // https://w3c.github.io/clipboard-apis/#clipboarditem-unsanitized-mime-types
    #[gc(untraced)]
    unsanitized_mime_types: RefCell<Vec<Utf16String>>,

    // https://w3c.github.io/clipboard-apis/#clipboarditem-representations-with-resolvers
    representations_with_resolvers: GcRefCell<Vec<(Utf16String, WebIdlPromise)>>,
}

impl Wrappable for ClipboardItem {}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct GetTypeInParallel {
    item: Ref<ClipboardItem>,
    realm: Gc<Realm>,
    promise: WebIdlPromise,
    #[gc(untraced)]
    mime_type: MimeType,
    is_custom: bool,
}

fn not_found_error(r#type: &Utf16String) -> Exception {
    Exception::Dom(
        "NotFoundError",
        Utf16String::from_utf8(&format!(
            "No data found for MIME type: {}",
            String::from_utf16_lossy(&r#type.to_utf16())
        )),
    )
}

fn without_web_custom_format_prefix(r#type: &Utf16String) -> (Vec<char>, bool) {
    let code_points = string_to_code_points(r#type);
    let prefix: Vec<char> = WEB_CUSTOM_FORMAT_PREFIX.chars().collect();
    match code_points.strip_prefix(prefix.as_slice()) {
        Some(rest) => (rest.to_vec(), true),
        None => (code_points, false),
    }
}

fn resolve_with_blob(vm: &Vm, realm: Gc<Realm>, promise: WebIdlPromise, blob: Ref<Blob>) {
    let wrapper = wrap(&CallContext::new(vm, realm), blob);
    resolve_promise(vm, promise, Value::from_object(wrapper));
}

impl ClipboardItem {
    pub(crate) fn create(vm: &Vm) -> Ref<ClipboardItem> {
        let item = allocate(vm, |_| ClipboardItem {
            this: Cell::new(None),
            representations: GcRefCell::new(Vec::new()),
            presentation_style: Cell::new(PresentationStyle::Unspecified),
            types: RefCell::new(Vec::new()),
            clipboard_change_count_at_read: Cell::new(None),
            originating_system_clipboard_item: RefCell::new(None),
            unsanitized_mime_types: RefCell::new(Vec::new()),
            representations_with_resolvers: GcRefCell::new(Vec::new()),
        });
        item.this.set(Some(item));
        item
    }

    fn this(&self) -> Ref<Self> {
        self.this
            .get()
            .expect("a ClipboardItem knows itself once it is created")
    }

    // https://w3c.github.io/clipboard-apis/#dom-clipboarditem-clipboarditem
    pub(crate) fn construct(
        cx: &mut CallContext,
        items: MarkedVec<'_, (Utf16String, WebIdlPromise)>,
        options: ClipboardItemOptions,
    ) -> WebIdlResult<Ref<ClipboardItem>> {
        let vm = cx.vm();

        // 1. If items is empty, then throw a TypeError.
        if items.is_empty() {
            return Err(Exception::type_error("Items cannot be empty"));
        }

        // 2. If options is empty, then set options["presentationStyle"] = "unspecified".
        // NOTE: This step is handled by presentationStyle's default value in ClipboardItemOptions.

        // 3. Set this's clipboard item to a new clipboard item.
        let clipboard_item = Self::create(vm);

        // 4. Set this's clipboard item's presentation style to options["presentationStyle"].
        clipboard_item.presentation_style.set(options.presentation_style);

        // 5. Let types be a list of DOMString.
        let mut types = Vec::new();

        // 6. For each (key, value) in items:
        for (key, value) in items.values().iter() {
            // 2. Let isCustom be false.
            // 3. If key starts with `"web "` prefix, then:
            //     1. Remove `"web "` prefix and assign the remaining string to key.
            //     2. Set isCustom to true.
            let (key_without_prefix, is_custom) = without_web_custom_format_prefix(key);

            // 5. Let mimeType be the result of parsing a MIME type given key.
            // 6. If mimeType is failure, then throw a TypeError.
            let Some(mime_type) = MimeType::parse(&key_without_prefix) else {
                return Err(Exception::TypeError(Utf16String::from_utf8(&format!(
                    "Invalid MIME type: {}",
                    String::from_utf16_lossy(&key.to_utf16())
                ))));
            };

            let mime_type_serialized = Utf16String::from_utf8(&mime_type.serialize());

            // 7. If this's clipboard item's list of representations contains a representation whose MIME type
            //    is mimeType and whose [representation/isCustom] is isCustom, then throw a TypeError.
            let exists = clipboard_item
                .representations
                .borrow()
                .iter()
                .any(|item| item.mime_type == mime_type_serialized && item.is_custom == is_custom);
            if exists {
                return Err(Exception::TypeError(Utf16String::from_utf8(&format!(
                    "Duplicate MIME type: {}",
                    String::from_utf16_lossy(&key.to_utf16())
                ))));
            }

            // 11. Let mimeTypeString be the result of serializing a MIME type with mimeType.
            // 12. If isCustom is true, prefix mimeTypeString with `"web "`.
            let mime_type_string = if is_custom {
                Utf16String::from_utf8(&format!("web {}", mime_type.serialize()))
            } else {
                mime_type_serialized.clone()
            };

            // 13. Add mimeTypeString to types.
            types.push(mime_type_string);

            // 1. Let representation be a new representation.
            // 4. Set representation’s isCustom flag to isCustom.
            // 8. Set representation’s MIME type to mimeType.
            // 9. Set representation’s data to value.
            // 10. Append representation to this's clipboard item's list of representations.
            clipboard_item.representations.borrow_mut(vm).push(Representation {
                mime_type: mime_type_serialized,
                is_custom,
                data: *value,
            });
        }

        // 7. Set this's types array to the result of running create a frozen array from types.
        *clipboard_item.types.borrow_mut() = types;

        Ok(clipboard_item)
    }

    pub(crate) fn presentation_style(&self, _: &mut CallContext) -> WebIdlResult<PresentationStyle> {
        Ok(self.presentation_style.get())
    }

    pub(crate) fn types(&self, _: &mut CallContext) -> WebIdlResult<Vec<Utf16String>> {
        Ok(self.types.borrow().clone())
    }

    pub(crate) fn representations<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Representation> {
        let representations = MarkedVec::new(vm);
        representations.extend_from_slice(&self.representations.borrow());
        representations
    }

    pub(crate) fn has_representations(&self) -> bool {
        !self.representations.borrow().is_empty()
    }

    pub(crate) fn append_representation(&self, vm: &Vm, representation: Representation) {
        self.types.borrow_mut().push(representation.mime_type.clone());
        self.representations.borrow_mut(vm).push(representation);
    }

    pub(crate) fn set_clipboard_change_count_at_read(&self, count: usize) {
        self.clipboard_change_count_at_read.set(Some(count));
    }

    pub(crate) fn set_originating_system_clipboard_item(&self, item: Option<SystemClipboardItem>) {
        *self.originating_system_clipboard_item.borrow_mut() = item;
    }

    pub(crate) fn set_unsanitized_mime_types(&self, types: Vec<Utf16String>) {
        *self.unsanitized_mime_types.borrow_mut() = types;
    }

    // https://w3c.github.io/clipboard-apis/#dom-clipboarditem-gettype
    pub(crate) fn get_type(&self, cx: &mut CallContext, r#type: Utf16String) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 2. Let isCustom be false.
        // 3. If type starts with `"web "` prefix, then:
        //     1. Remove `"web "` prefix and assign the remaining string to type.
        //     2. Set isCustom to true.
        let (type_without_prefix, is_custom) = without_web_custom_format_prefix(&r#type);

        // 4. Let mimeType be the result of parsing a MIME type given type.
        // 5. If mimeType is failure, then throw a TypeError.
        let Some(mime_type) = MimeType::parse(&type_without_prefix) else {
            return Err(Exception::TypeError(Utf16String::from_utf8(&format!(
                "Invalid MIME type: {}",
                String::from_utf16_lossy(&r#type.to_utf16())
            ))));
        };

        let mime_type_serialized = Utf16String::from_utf8(&mime_type.serialize());

        // 6. Let itemTypeList be this’s clipboard item’s list of representations.
        let item_type_list = self.representations(vm);

        // 7.  Let p be a new promise in realm.
        let promise = create_promise(vm, realm);

        // 8. For each representation in itemTypeList:
        for (index, representation) in item_type_list.values().iter().enumerate() {
            // 1. If representation’s MIME type is mimeType and representation’s isCustom is isCustom, then:
            if representation.mime_type == mime_type_serialized && representation.is_custom == is_custom {
                // FIXME: 1. If this’s clipboard change count at read is not null, and the current clipboard change count is not
                //           equal to this’s clipboard change count at read, then reject p with an "InvalidStateError" DOMException
                //           in realm, and return p.

                // 2. If this’s clipboard change count at read is not null, then:
                if self.clipboard_change_count_at_read.get().is_some() {
                    // 1. Let key be mimeType’s essence. If isCustom is true, prefix key with `"web "`.
                    let essence = mime_type.essence();
                    let mut key = format!("{}/{}", essence.type_, essence.subtype);
                    if is_custom {
                        key = format!("web {key}");
                    }
                    let key = Utf16String::from_utf8(&key);

                    // 2. If this’s representations with resolvers[key] exists, then return this’s representations with resolvers[key].
                    let resolver = self
                        .representations_with_resolvers
                        .borrow()
                        .iter()
                        .find(|(existing_key, _)| *existing_key == key)
                        .map(|(_, resolver)| *resolver);
                    if let Some(resolver) = resolver {
                        return Ok(resolver);
                    }

                    // 3. Set this’s representations with resolvers[key] to p.
                    self.representations_with_resolvers.borrow_mut(vm).push((key, promise));

                    // 4. Run the following steps in parallel:
                    deferred_invoke(
                        vm,
                        GetTypeInParallel {
                            item: self.this(),
                            realm,
                            promise,
                            mime_type: mime_type.clone(),
                            is_custom,
                        },
                        get_type_in_parallel,
                    );
                }

                // 3. Let representationDataPromise be the representation’s data.
                let representation_data_promise = representation.data;

                // 4. React to representationDataPromise:
                react_to_promise(
                    vm,
                    representation_data_promise,
                    (promise, self.this(), index),
                    // 1. If representationDataPromise was fulfilled with value v, then:
                    Some(|cx, (promise, item, index), value| {
                        let vm = cx.vm();

                        // 1. If v is a DOMString, then follow the below steps:
                        if value.is_string() {
                            // 1. Let dataAsBytes be the result of UTF-8 encoding v.
                            let data_as_bytes =
                                String::from_utf16_lossy(&value.as_string().utf16_string().to_utf16()).into_bytes();

                            // 2. Let blobData be a Blob created using dataAsBytes with its type set to mimeType, serialized.
                            let mime_type_serialized = item.representations.borrow()[index].mime_type.clone();
                            let blob_data = allocate(vm, |_| Blob::new(data_as_bytes, mime_type_serialized));

                            // 3. Resolve p with blobData.
                            resolve_with_blob(vm, promise_realm(promise), promise, blob_data);
                        }
                        // 2. If v is a Blob, then follow the below steps:
                        if clipboard_item_data_blob(value).is_some() {
                            // 1. Resolve p with v.
                            resolve_promise(vm, promise, value);
                        }

                        Ok(Value::UNDEFINED)
                    }),
                    // 2. If representationDataPromise was rejected, then:
                    Some(|cx, (promise, item, index), _| {
                        // 1. Reject p with "NotFoundError" DOMException in realm.
                        let r#type = item.representations.borrow()[index].type_string();
                        reject_promise_with_exception(cx.vm(), promise, not_found_error(&r#type));

                        Ok(Value::UNDEFINED)
                    }),
                );

                // 5. Return p.
                return Ok(promise);
            }
        }

        // 9. Reject p with "NotFoundError" DOMException in realm.
        reject_promise_with_exception(vm, promise, not_found_error(&r#type));

        // 10. Return p.
        Ok(promise)
    }

    pub(crate) fn supports(_: &mut CallContext, r#type: Utf16String) -> WebIdlResult<bool> {
        Ok(Self::supports_type(&r#type))
    }

    // https://w3c.github.io/clipboard-apis/#dom-clipboarditem-supports
    pub(crate) fn supports_type(r#type: &Utf16String) -> bool {
        // 1. If type is in mandatory data types or optional data types, then return true.
        // 2. If not, then return false.
        // TODO: Implement optional data types, like web custom formats and image/svg+xml.
        let r#type = r#type.to_utf16();
        ["text/plain", "text/html", "image/png"]
            .iter()
            .any(|supported| r#type.iter().copied().eq(supported.encode_utf16()))
    }
}

// https://w3c.github.io/clipboard-apis/#dom-clipboarditem-gettype
fn get_type_in_parallel(vm: &Vm, steps: GetTypeInParallel) {
    let GetTypeInParallel {
        item,
        realm,
        promise,
        mime_type,
        is_custom,
    } = steps;

    // 1. Let clipboardItem be this’s originating system clipboard item.
    let clipboard_item = item.originating_system_clipboard_item.borrow();

    // 2. If clipboardItem is null, then queue a global task on the clipboard task source, given realm’s
    //    global object, to reject p with an "InvalidStateError" DOMException in realm, then abort these
    //    steps.
    let Some(clipboard_item) = clipboard_item.as_ref() else {
        queue_a_global_task(
            vm,
            realm.global_object(),
            TaskSource::Clipboard,
            promise,
            |vm, promise| {
                with_temporary_execution_context(promise_realm(promise), || {
                    reject_promise_with_exception(
                        vm,
                        promise,
                        Exception::Dom(
                            "InvalidStateError",
                            Utf16String::from_utf8(
                                "This clipboard item is missing an originating system clipboard entry",
                            ),
                        ),
                    );
                });
            },
        );
        return;
    };

    let mut os_format_name = String::new();

    // 3. If isCustom is true, then:
    if is_custom {
        // FIXME: 1. Let mapName be the os specific custom map name.
        // FIXME: 2. Let mapRepresentation be the system clipboard representation in clipboardItem’s list of system
        //           clipboard representations whose name is mapName. If there is no such system clipboard
        //           representation, then queue a global task on the clipboard task source, given realm’s global
        //           object, to reject p with a "NotFoundError" DOMException in realm, then abort these steps.
        // FIXME: 3. Let webCustomFormatMapString be the JSON string deserialized from mapRepresentation’s data.
        // FIXME: 4. Let osFormatName be the value in webCustomFormatMapString whose key matches mimeType
        //           serialized.
        // FIXME: 5. If osFormatName is not found, then queue a global task on the clipboard task source, given
        //           realm’s global object, to reject p with a "NotFoundError" DOMException in realm, then abort
        //           these steps.
    }
    // 4. Else, let osFormatName be the result of running os specific well-known format given mimeType.
    else {
        os_format_name = os_specific_well_known_format(&mime_type);
    }

    // 5. Let clipboardRepresentation be the system clipboard representation in clipboardItem’s list of
    //    system clipboard representations whose name is osFormatName. If there is no such system clipboard
    //    representation, then queue a global task on the clipboard task source, given realm’s global object,
    //    to reject p with a "NotFoundError" DOMException in realm, then abort these steps.
    let clipboard_representation = clipboard_item
        .system_clipboard_representations
        .iter()
        .find(|item| item.name == os_format_name);

    let Some(clipboard_representation) = clipboard_representation else {
        let message = Utf16String::from_utf8(&format!("No data found for MIME type: {os_format_name}"));
        queue_a_global_task(
            vm,
            realm.global_object(),
            TaskSource::Clipboard,
            (promise, message),
            |vm, (promise, message)| {
                with_temporary_execution_context(promise_realm(promise), || {
                    reject_promise_with_exception(vm, promise, Exception::Dom("NotFoundError", message));
                });
            },
        );
        return;
    };

    // 6. Let rawData be clipboardRepresentation’s data.
    let raw_data = &clipboard_representation.data;

    // 7. Let cleanData be a copy of rawData.
    let clean_data = raw_data.clone();

    // FIXME: 8. If mimeType’s essence is in this’s unsanitized MIME types and mimeType’s essence is in the
    //           optional unsanitized data types list, then do nothing.
    // FIXME: 9. Else, if mimeType’s essence is not "image/png", the user agent MAY sanitize cleanData.

    // 10. Let blob be a Blob whose type is mimeType serialized and whose underlying byte sequence is cleanData.
    let blob = allocate(vm, |_| {
        Blob::new(clean_data, Utf16String::from_utf8(&mime_type.serialize()))
    });

    // 11. Queue a global task on the clipboard task source, given realm’s global object, to resolve p with blob.
    queue_a_global_task(
        vm,
        realm.global_object(),
        TaskSource::Clipboard,
        (realm, promise, blob),
        |vm, (realm, promise, blob)| {
            with_temporary_execution_context(realm, || {
                resolve_with_blob(vm, realm, promise, blob);
            });
        },
    );
}
