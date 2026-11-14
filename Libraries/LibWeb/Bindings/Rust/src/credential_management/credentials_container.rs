/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CredentialsContainer, https://w3c.github.io/webappsec-credential-management/#credentialscontainer

use core::ffi::c_void;

use libjs_rust::api::{ErrorKind, ErrorType, Trace, Vm};

use super::credential::{AnyCredential, CredentialMediationRequirement};
use super::federated_credential::{FederatedCredentialInit, FederatedCredentialRequestOptions};
use super::password_credential::PasswordCredentialInit;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::promise::{WebIdlPromise, reject_promise_with_exception};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::abort_signal::AbortSignal;

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialrequestoptions
#[expect(dead_code, reason = "get() does not read the options yet")]
pub(crate) struct CredentialRequestOptions {
    pub(crate) federated: Option<FederatedCredentialRequestOptions>,
    pub(crate) mediation: CredentialMediationRequirement,
    pub(crate) password: bool,
    pub(crate) signal: Option<Ref<AbortSignal>>,
}

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialcreationoptions
#[expect(dead_code, reason = "create() does not read the options yet")]
pub(crate) struct CredentialCreationOptions {
    pub(crate) federated: Option<FederatedCredentialInit>,
    pub(crate) mediation: CredentialMediationRequirement,
    pub(crate) password: Option<PasswordCredentialInit>,
    pub(crate) signal: Option<Ref<AbortSignal>>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CredentialsContainer;

impl Wrappable for CredentialsContainer {}

fn reject_not_implemented_promise(vm: &Vm, promise: WebIdlPromise, operation: &str) {
    let throw = vm
        .throw_completion::<()>(ErrorKind::InternalError, ErrorType::NotImplemented, &[&operation])
        .expect_err("throwing a completion throws");
    reject_promise_with_exception(vm, promise, Exception::from(throw));
}

impl CredentialsContainer {
    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-get
    pub(crate) fn get(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        _: CredentialRequestOptions,
    ) -> WebIdlResult<()> {
        eprintln!("FIXME: Unimplemented CredentialsContainer::get()");
        reject_not_implemented_promise(cx.vm(), promise, "get");
        Ok(())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-store
    pub(crate) fn store(&self, cx: &mut CallContext, promise: WebIdlPromise, _: AnyCredential) -> WebIdlResult<()> {
        eprintln!("FIXME: Unimplemented CredentialsContainer::store()");
        reject_not_implemented_promise(cx.vm(), promise, "store");
        Ok(())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-create
    pub(crate) fn create(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        _: CredentialCreationOptions,
    ) -> WebIdlResult<()> {
        eprintln!("FIXME: Unimplemented CredentialsContainer::create()");
        reject_not_implemented_promise(cx.vm(), promise, "create");
        Ok(())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-preventsilentaccess
    pub(crate) fn prevent_silent_access(&self, cx: &mut CallContext, promise: WebIdlPromise) -> WebIdlResult<()> {
        eprintln!("FIXME: Unimplemented CredentialsContainer::prevent_silent_access()");
        reject_not_implemented_promise(cx.vm(), promise, "prevent silent access");
        Ok(())
    }
}

/// # Safety
///
/// The VM must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_credentials_container_create(vm: *const c_void) -> *mut c_void {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(allocate(vm, |_| CredentialsContainer))
}
