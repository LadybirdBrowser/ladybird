/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! XMLHttpRequestUpload, https://xhr.spec.whatwg.org/#xmlhttprequestupload

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Trace, Vm};

use super::xml_http_request_event_target::XMLHttpRequestEventTarget;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};

// https://xhr.spec.whatwg.org/#xmlhttprequestupload
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct XMLHttpRequestUpload {
    target: XMLHttpRequestEventTarget,
}

impl Inherits<XMLHttpRequestEventTarget> for XMLHttpRequestUpload {
    fn parent(&self) -> &XMLHttpRequestEventTarget {
        &self.target
    }
}

impl Wrappable for XMLHttpRequestUpload {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.target.cpp_parent()
    }
}

impl GetTheParent for XMLHttpRequestUpload {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl XMLHttpRequestUpload {
    pub(crate) fn create(vm: &Vm) -> Ref<Self> {
        let upload = allocate(vm, |_| Self {
            target: XMLHttpRequestEventTarget::new(),
        });
        upload.event_target().set_owner(upload);
        upload
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        self.target.event_target()
    }
}
