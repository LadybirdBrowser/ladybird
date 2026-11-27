/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSTransition, https://drafts.csswg.org/css-transitions-2/#the-CSSTransition-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Visitor, Vm};

use crate::animations::animation::Animation;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://drafts.csswg.org/css-transitions-2/#the-CSSTransition-interface
pub(crate) struct CSSTransition {
    animation: Animation,
}

// SAFETY: The animation visits what it reaches.
unsafe impl Trace for CSSTransition {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        self.animation.trace(visitor);
    }
}

impl Inherits<Animation> for CSSTransition {
    fn parent(&self) -> &Animation {
        &self.animation
    }
}

impl Wrappable for CSSTransition {
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        self.animation.relevant_realm()
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.animation.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        object.animation.finalize_animation();
    }
}

impl GetTheParent for CSSTransition {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl CSSTransition {
    /// The object of a CSS transition, whose state `animation` holds.
    pub(crate) fn allocate(vm: &Vm, animation: Animation) -> Ref<Self> {
        allocate(vm, |_| Self { animation })
    }

    // https://drafts.csswg.org/css-transitions-2/#dom-csstransition-transitionproperty
    pub(crate) fn transition_property(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.animation.record().css_transition_property())
    }
}
