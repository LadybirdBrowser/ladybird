/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.csswg.org/web-animations-1/#extensions-to-the-documentorshadowroot-interface-mixin

use libjs_rust::api::MarkedVec;

use super::animatable::sorted_relevant_animations;
use crate::bindings::conversions::CppInterface;
use crate::bindings::{CallContext, WebIdlResult};

/// https://drafts.csswg.org/web-animations-1/#dom-documentorshadowroot-getanimations
pub(crate) fn get_animations<'vm>(
    this: &CppInterface,
    cx: &mut CallContext<'vm>,
) -> WebIdlResult<MarkedVec<'vm, super::animation::AnyAnimation>> {
    // Returns the set of relevant animations for a subtree for the document or shadow root on which this method is
    // called.
    // NB: LibWeb updates the style of the document before it collects the animations.
    // The returned list is sorted using the composite order described for the associated animations of effects in
    // § 5.4.2 The effect stack.
    let animations = sorted_relevant_animations(this, true, cx);

    // Calling this method triggers a style change event for the document. As a result, the returned list reflects
    // the state after applying any pending style changes to animation such as changes to animation-related style
    // properties that have yet to be processed.
    // FIXME: Implement this.

    Ok(animations)
}
