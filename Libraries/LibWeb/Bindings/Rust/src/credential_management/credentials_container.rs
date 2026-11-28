/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CredentialsContainer, https://w3c.github.io/webappsec-credential-management/#credentialscontainer

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{ErrorKind, ErrorType, Trace, Value, Vm};

use super::credential::{AnyCredential, CredentialMediationRequirement};
use super::federated_credential::{FederatedCredentialInit, FederatedCredentialRequestOptions};
use super::password_credential::PasswordCredentialInit;
use crate::bindings::environment_settings_object::current_settings_object;
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::promise::{WebIdlPromise, reject_promise, reject_promise_with_exception, resolve_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::abort_signal::AbortSignal;
use crate::permissions_policy::permissions_policy::PolicyControlledFeature;
use crate::web_authentication::ceremony::{
    self, Caller, discover_from_external_source, is_same_origin_with_its_ancestors,
};
use crate::web_authentication::public_key_credential::{
    PublicKeyCredentialCreationOptions, PublicKeyCredentialRequestOptions,
};

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialrequestoptions
pub(crate) struct CredentialRequestOptions<'vm> {
    pub(crate) federated: Option<FederatedCredentialRequestOptions>,
    pub(crate) mediation: CredentialMediationRequirement,
    pub(crate) password: bool,
    pub(crate) public_key: Option<PublicKeyCredentialRequestOptions<'vm>>,
    pub(crate) signal: Option<Ref<AbortSignal>>,
}

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialcreationoptions
pub(crate) struct CredentialCreationOptions<'vm> {
    pub(crate) federated: Option<FederatedCredentialInit>,
    pub(crate) mediation: CredentialMediationRequirement,
    pub(crate) password: Option<PasswordCredentialInit>,
    pub(crate) public_key: Option<PublicKeyCredentialCreationOptions<'vm>>,
    pub(crate) signal: Option<Ref<AbortSignal>>,
}

#[derive(Trace)]
pub(crate) struct CredentialsContainer;

impl Wrappable for CredentialsContainer {}

fn reject_not_implemented_promise(vm: &Vm, promise: WebIdlPromise, operation: &str) {
    let throw = vm
        .throw_completion::<()>(ErrorKind::InternalError, ErrorType::NotImplemented, &[&operation])
        .expect_err("throwing a completion throws");
    reject_promise_with_exception(vm, promise, Exception::from(throw));
}

fn not_supported_error(message: &str) -> Exception {
    Exception::Dom("NotSupportedError", Utf16String::from_utf8(message))
}

impl CredentialsContainer {
    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-get
    pub(crate) fn get(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        options: CredentialRequestOptions<'_>,
    ) -> WebIdlResult<()> {
        // https://w3c.github.io/webappsec-credential-management/#abstract-opdef-request-a-credential
        let vm = cx.vm();

        // 1. Let settings be the current settings object.
        let settings = current_settings_object(vm);

        // 2. Assert: settings is a secure context.
        debug_assert!(settings.is_secure_context());

        // 3. Let document be settings's relevant global object's associated Document.
        let Some(window) = window_from_global_object(settings.global_object()) else {
            return Err(not_supported_error("Credentials need a window"));
        };
        let document = window.associated_document();

        // 4. If document is not fully active, then return a promise rejected with an "InvalidStateError"
        //    DOMException.
        if !document.is_fully_active() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("The document is not fully active"),
            ));
        }

        // 5. If options.signal is aborted, then return a promise rejected with options.signal's abort reason.
        if let Some(signal) = &options.signal
            && signal.aborted()
        {
            reject_promise(vm, promise, signal.reason());
            return Ok(());
        }

        // 6. Let interfaces be options's relevant credential interface objects.
        // 7. If interfaces is empty, then return a promise rejected with a "NotSupportedError" DOMException.
        // FIXME: Request the credentials of the other interfaces. A request that also names public key credentials
        //        gets those.
        let Some(public_key) = &options.public_key else {
            if options.password || options.federated.is_some() {
                eprintln!("FIXME: Unimplemented CredentialsContainer::get() for password and federated credentials");
                reject_not_implemented_promise(vm, promise, "get");
                return Ok(());
            }
            return Err(not_supported_error("The options name no credential type"));
        };

        // 9. Let origin be settings' origin.
        let origin = settings.origin();

        // 10. For each interface of interfaces:
        //     1. If instances of interface are origin bound and origin is an opaque origin, then return a promise
        //        rejected with a "SecurityError" DOMException.
        // NB: PublicKeyCredential's [[DiscoverFromExternalSource]] has this check, with a "NotAllowedError".

        // FIXME: 12.3. If settings' active credential types contains interface's [[type]], return a promise rejected
        //              with a "NotAllowedError" DOMException.

        // 13. Let sameOriginWithAncestors be true if settings is same-origin with its ancestors, and false otherwise.
        let same_origin_with_ancestors = is_same_origin_with_its_ancestors(&document);

        // 14. For each interface in options' relevant credential interface objects:
        //     1. Let permission be the interface's [[type]] Get Permissions Policy.
        //     2. If permission is null, continue.
        //     3. If document is not allowed to use permission, return a promise rejected with a "NotAllowedError"
        //        DOMException.
        if !document.is_allowed_to_use_feature(PolicyControlledFeature::PublickeyCredentialsGet) {
            return Err(Exception::Dom(
                "NotAllowedError",
                Utf16String::from_utf8("The publickey-credentials-get feature is not allowed in this document"),
            ));
        }

        // 16. Let p be a new promise.
        // 17. Run the following steps in parallel:
        //     1. Let credentials be the result of collecting Credentials from the credential store, given origin,
        //        options, and sameOriginWithAncestors.
        // NB: PublicKeyCredential's [[CollectFromCredentialStore]] returns an empty set.
        //     4. If options' mediation is "silent", resolve p with null, and skip the remaining steps.
        if matches!(options.mediation, CredentialMediationRequirement::Silent) {
            resolve_promise(vm, promise, Value::NULL);
            return Ok(());
        }
        //     5. Let result be the result of asking the user to choose a Credential, given options and credentials.
        // NB: For a public key credential, that is its [[DiscoverFromExternalSource]] internal method.
        let caller = Caller {
            window: &window,
            origin: &origin,
            same_origin_with_ancestors,
        };
        discover_from_external_source(vm, &caller, promise, public_key, options.mediation, options.signal)
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-store
    pub(crate) fn store(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        credential: AnyCredential,
    ) -> WebIdlResult<()> {
        // https://w3c.github.io/webauthn/#sctn-storeCredential
        // The [[Store]](credential, sameOriginWithAncestors) method is not supported for Web Authentication's
        // PublicKeyCredential type, so its implementation of the [[Store]](credential, sameOriginWithAncestors)
        // internal method always throws an error.
        if matches!(credential, AnyCredential::PublicKeyCredential(_)) {
            return Err(not_supported_error("A public key credential cannot be stored"));
        }
        eprintln!("FIXME: Unimplemented CredentialsContainer::store()");
        reject_not_implemented_promise(cx.vm(), promise, "store");
        Ok(())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-create
    pub(crate) fn create(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        options: CredentialCreationOptions<'_>,
    ) -> WebIdlResult<()> {
        // https://w3c.github.io/webappsec-credential-management/#abstract-opdef-create-a-credential
        let vm = cx.vm();

        // 1. Let settings be the current settings object.
        let settings = current_settings_object(vm);

        // 2. Assert: settings is a secure context.
        debug_assert!(settings.is_secure_context());

        // 3. Let global be settings' global object.
        // 4. Let document be the relevant global object's associated Document.
        // 9.1. global does not have an associated Document.
        let Some(window) = window_from_global_object(settings.global_object()) else {
            return Err(not_supported_error("Credentials need a window"));
        };
        let document = window.associated_document();

        // 5. If document is not fully active, then return a promise rejected with an "InvalidStateError"
        //    DOMException.
        if !document.is_fully_active() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("The document is not fully active"),
            ));
        }

        // 6. If options.signal is aborted, then return a promise rejected with options.signal's abort reason.
        if let Some(signal) = &options.signal
            && signal.aborted()
        {
            reject_promise(vm, promise, signal.reason());
            return Ok(());
        }

        // 7. Let sameOriginWithAncestors be true if the current settings object is same-origin with its ancestors,
        //    and false otherwise.
        let same_origin_with_ancestors = is_same_origin_with_its_ancestors(&document);

        // 8. Let interfaces be the set of options' relevant credential interface objects.
        // 9. Return a promise rejected with a "NotSupportedError" DOMException if any of the following statements
        //    are true:
        //    2. interfaces is empty.
        //    3. interfaces' size is greater than 1.
        let interface_count = usize::from(options.federated.is_some())
            + usize::from(options.password.is_some())
            + usize::from(options.public_key.is_some());
        if interface_count != 1 {
            return Err(not_supported_error("The options must name one credential type"));
        }
        // FIXME: Create the credentials of the other interfaces.
        let Some(public_key) = &options.public_key else {
            eprintln!("FIXME: Unimplemented CredentialsContainer::create() for password and federated credentials");
            reject_not_implemented_promise(vm, promise, "create");
            return Ok(());
        };

        // 10. For each interface in interfaces:
        //     1. Let permission be the interface's [[type]] Create Permissions Policy.
        //     2. If permission is null, continue.
        //     3. If document is not allowed to use permission, return a promise rejected with a "NotAllowedError"
        //        DOMException.
        if !document.is_allowed_to_use_feature(PolicyControlledFeature::PublickeyCredentialsCreate) {
            return Err(Exception::Dom(
                "NotAllowedError",
                Utf16String::from_utf8("The publickey-credentials-create feature is not allowed in this document"),
            ));
        }

        // 11. Let origin be settings' origin.
        let origin = settings.origin();

        // 12. For each interface of interfaces:
        //     1. If instances of interface are origin bound and origin is an opaque origin, return a promise rejected
        //        with a "SecurityError" DOMException.
        // NB: PublicKeyCredential's [[Create]] has this check, with a "NotAllowedError".

        // FIXME: 15. If settings' active credential types contains type, return a promise rejected with a
        //            "NotAllowedError" DOMException.

        // 17. Let p be a new promise.
        // 18. Run the following steps in parallel:
        //     1. Let r be the result of executing interfaces[0]'s [[Create]](origin, options, sameOriginWithAncestors)
        //        internal method on origin, options, and sameOriginWithAncestors.
        let caller = Caller {
            window: &window,
            origin: &origin,
            same_origin_with_ancestors,
        };
        ceremony::create(vm, &caller, promise, public_key, options.mediation, options.signal)
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-preventsilentaccess
    pub(crate) fn prevent_silent_access(&self, cx: &mut CallContext, promise: WebIdlPromise) -> WebIdlResult<()> {
        eprintln!("FIXME: Unimplemented CredentialsContainer::prevent_silent_access()");
        reject_not_implemented_promise(cx.vm(), promise, "prevent silent access");
        Ok(())
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_credentials_container_create(vm: &Vm) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| CredentialsContainer))
}
