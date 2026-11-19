/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The members of Navigator that are implemented in Rust, https://html.spec.whatwg.org/multipage/system-state.html#navigator

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Value};
use liburl_rust::origin::Origin;

use crate::bindings::conversions::CppInterface;
use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{with_temporary_execution_context_and_callbacks, wrap};
use crate::bindings::promise::{
    WebIdlPromise, deferred_invoke, promise_realm, reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::encrypted_media_extensions::algorithms::{
    get_supported_configuration, is_supported_key_system, key_system_from_string,
};
use crate::encrypted_media_extensions::media_key_system_access::{MediaKeySystemAccess, MediaKeySystemConfiguration};
use crate::permissions_policy::permissions_policy::PolicyControlledFeature;

unsafe extern "C" {
    fn web_html_navigator_window(navigator_wrapper: *mut c_void) -> *mut c_void;
}

/// https://html.spec.whatwg.org/multipage/system-state.html#navigator
fn window_of(navigator: &CppInterface) -> CppWindow {
    // SAFETY: The wrapper is a live Navigator wrapper, and LibWeb returns the live Window of the Navigator.
    unsafe { CppWindow::from_raw(web_html_navigator_window(navigator.wrapper().as_ptr().cast())) }
}

/// What the parallel steps of requestMediaKeySystemAccess() capture.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct RequestMediaKeySystemAccessCaptures {
    realm: Gc<Realm>,
    promise: WebIdlPromise,
    key_system: Utf16String,
    #[gc(untraced)]
    supported_configurations: Vec<MediaKeySystemConfiguration>,
    #[gc(untraced)]
    origin: Origin,
}

// https://w3c.github.io/encrypted-media/#navigator-extension-requestmediakeysystemaccess
pub(crate) fn request_media_key_system_access(
    this: &CppInterface,
    cx: &mut CallContext,
    promise: WebIdlPromise,
    key_system: Utf16String,
    supported_configurations: Vec<MediaKeySystemConfiguration>,
) -> WebIdlResult<()> {
    let vm = cx.vm();

    // 1. If this's relevant global object's associated Document is not allowed to use the encrypted-media feature, then throw a "SecurityError" DOMException and abort these steps.
    let associated_document = window_of(this).associated_document();
    if !associated_document.is_allowed_to_use_feature(PolicyControlledFeature::EncryptedMedia) {
        reject_promise_with_exception(
            vm,
            promise,
            Exception::Dom(
                "SecurityError",
                Utf16String::from_ascii(b"This document is not allowed to use the encrypted-media feature"),
            ),
        );
        return Ok(());
    }

    // 2. If keySystem is the empty string, return a promise rejected with a newly created TypeError.
    if key_system.is_empty() {
        reject_promise_with_exception(
            vm,
            promise,
            Exception::TypeError(Utf16String::from_ascii(b"keySystem must not be empty")),
        );
        return Ok(());
    }

    // 3. If supportedConfigurations is empty, return a promise rejected with a newly created TypeError.
    if supported_configurations.is_empty() {
        reject_promise_with_exception(
            vm,
            promise,
            Exception::TypeError(Utf16String::from_ascii(b"supportedConfigurations must not be empty")),
        );
        return Ok(());
    }

    // 4. Let document be the calling context's Document.
    // FIXME: Is this the same as the associated document?
    let document = associated_document;

    // 5. Let origin be the origin of document.
    let origin = document.origin();

    // 7. Run the following steps in parallel:
    let captures = RequestMediaKeySystemAccessCaptures {
        realm: promise_realm(promise),
        promise,
        key_system,
        supported_configurations,
        origin,
    };
    deferred_invoke(vm, captures, |vm, captures| {
        let RequestMediaKeySystemAccessCaptures {
            realm,
            promise,
            key_system,
            supported_configurations,
            origin,
        } = captures;
        with_temporary_execution_context_and_callbacks(realm, || {
            // 1. If keySystem is not one of the Key Systems supported by the user agent, reject promise with a NotSupportedError. String comparison is case-sensitive.
            if !is_supported_key_system(&key_system) {
                reject_promise_with_exception(
                    vm,
                    promise,
                    Exception::Dom("NotSupportedError", Utf16String::from_ascii(b"Unsupported key system")),
                );
                return;
            }

            // 2. Let implementation be the implementation of keySystem.
            let implementation = key_system_from_string(&key_system);

            // 3. For each value in supportedConfigurations:
            for candidate_configuration in &supported_configurations {
                // 1. Let candidate configuration be the value.

                // 2. Let supported configuration be the result of executing the Get Supported Configuration algorithm on implementation, candidate configuration, and origin.
                let supported_configuration =
                    get_supported_configuration(implementation.as_ref(), candidate_configuration, &origin);

                // 3. If supported configuration is not NotSupported, run the following steps:
                if let Some(supported_configuration) = supported_configuration {
                    // 1. Let access be a new MediaKeySystemAccess object, and initialize it as follows:
                    // 1. Set the keySystem attribute to keySystem.
                    // 2. Let the configuration value be supported configuration.
                    // 3. Let the cdm implementation value be implementation.
                    let access = MediaKeySystemAccess::create(
                        vm,
                        key_system,
                        supported_configuration
                            .configuration
                            .expect("a supported configuration has a configuration"),
                        implementation,
                    );

                    // 2. Resolve promise with access and abort the parallel steps of this algorithm.
                    let wrapper = wrap(&CallContext::new(vm, realm), access);
                    resolve_promise(vm, promise, Value::from_object(wrapper));
                    return;
                }
            }

            // 4. Reject promise with a NotSupportedError.
            reject_promise_with_exception(
                vm,
                promise,
                Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_ascii(b"No supported configurations found for the requested key system"),
                ),
            );
        });
    });

    Ok(())
}
