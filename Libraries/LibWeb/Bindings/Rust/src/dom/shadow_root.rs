/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The members of ShadowRoot that are implemented in Rust, https://dom.spec.whatwg.org/#interface-shadowroot

use libjs_rust::api::MarkedVec;

use crate::animations::animation::AnyAnimation;
use crate::animations::document_or_shadow_root;
use crate::bindings::conversions::CppInterface;
use crate::bindings::{CallContext, WebIdlResult};

/// https://drafts.csswg.org/web-animations-1/#dom-documentorshadowroot-getanimations
pub(crate) fn get_animations<'vm>(
    this: &CppInterface,
    cx: &mut CallContext<'vm>,
) -> WebIdlResult<MarkedVec<'vm, AnyAnimation>> {
    document_or_shadow_root::get_animations(this, cx)
}
