/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! XMLHttpRequestEventTarget, https://xhr.spec.whatwg.org/#xmlhttprequesteventtarget

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::Trace;

use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::Wrappable;

// https://xhr.spec.whatwg.org/#xmlhttprequesteventtarget
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct XMLHttpRequestEventTarget {
    event_target: CppEventTarget,
}

impl Wrappable for XMLHttpRequestEventTarget {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for XMLHttpRequestEventTarget {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl XMLHttpRequestEventTarget {
    pub(crate) fn new() -> Self {
        Self {
            event_target: CppEventTarget::new(),
        }
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }
}
