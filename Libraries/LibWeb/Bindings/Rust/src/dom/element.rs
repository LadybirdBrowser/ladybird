/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The members of Element that are implemented in Rust, https://dom.spec.whatwg.org/#interface-element

use core::ffi::c_void;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Gc, MarkedVec, Object, Trace};

use crate::animations::animatable::{self, GetAnimationsOptions, UnrestrictedDoubleOrKeyframeAnimationOptions};
use crate::animations::animation::AnyAnimation;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_dom_is_valid_attribute_local_name(name: usize) -> bool;
    fn web_dom_element_local_name(element: *mut c_void) -> usize;
    fn web_dom_element_get_attribute_ns(element: *mut c_void, namespace: usize, name: usize) -> usize;
    fn web_dom_element_set_attribute_value(element: *mut c_void, name: usize, value: usize);
    fn web_dom_element_remove_attribute(element: *mut c_void, name: usize);
    fn web_dom_element_for_each_attribute(
        element: *mut c_void,
        callback: *mut c_void,
        on_attribute: unsafe extern "C" fn(*mut c_void, usize, usize),
    );
    fn web_dom_element_attribute(
        element: *mut c_void,
        name: ak::Span<'_, u8>,
        value: *mut c_void,
        on_value: unsafe extern "C" fn(*mut c_void, usize),
    );
}

unsafe extern "C" fn on_value(value: *mut c_void, raw_value: usize) {
    // SAFETY: value is the result slot of the caller, and LibWeb hands over a reference to the string.
    unsafe { *value.cast::<Option<Utf16String>>() = Some(Utf16String::from_raw_owned(raw_value)) };
}

unsafe extern "C" fn on_attribute(callback: *mut c_void, name: usize, value: usize) {
    // SAFETY: callback is the callback of the caller, and LibWeb lends the name and the value for the call.
    let (callback, name, value) = unsafe {
        (
            &mut *callback.cast::<&mut dyn FnMut(&Utf16FlyString, &Utf16String)>(),
            Utf16FlyString::from_raw_borrowed(name),
            Utf16String::from_raw_borrowed(value),
        )
    };
    callback(&name, &value);
}

/// https://dom.spec.whatwg.org/#concept-element
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppElement(CppCell);

impl CppElement {
    pub(crate) fn from_lent(element: &LentCppCell<cpp_class::Element>) -> Self {
        Self(CppCell::from_lent(element))
    }

    /// # Safety
    ///
    /// `element` must be a live Element.
    pub(crate) unsafe fn from_raw(element: *mut c_void) -> Self {
        // SAFETY: The caller passes a live Element.
        Self(unsafe { CppCell::from_raw(element) })
    }

    pub(crate) fn local_name(&self) -> Utf16FlyString {
        // SAFETY: The element is live and owns the returned local name.
        unsafe { Utf16FlyString::from_raw_borrowed(web_dom_element_local_name(self.0.as_ptr())) }
    }

    pub(crate) fn get_attribute_ns(
        &self,
        namespace: Option<&Utf16FlyString>,
        name: &Utf16FlyString,
    ) -> Option<Utf16String> {
        let namespace = namespace.map_or(0, |namespace| namespace.raw_identity());
        // SAFETY: The element is live and the names are lent for the call.
        let value = unsafe { web_dom_element_get_attribute_ns(self.0.as_ptr(), namespace, name.raw_identity()) };
        // SAFETY: A nonzero result transfers a reference to the value.
        (value != 0).then(|| unsafe { Utf16String::from_raw_owned(value) })
    }

    pub(crate) fn set_attribute_value(&self, name: &Utf16FlyString, value: Utf16String) {
        // SAFETY: The element is live, the name is lent for the call, and the value reference is transferred.
        unsafe { web_dom_element_set_attribute_value(self.0.as_ptr(), name.raw_identity(), value.into_raw()) };
    }

    /// https://dom.spec.whatwg.org/#concept-element-attributes-remove-by-name
    pub(crate) fn remove_attribute(&self, name: &Utf16FlyString) {
        // SAFETY: The element is live and the name is lent for the call.
        unsafe { web_dom_element_remove_attribute(self.0.as_ptr(), name.raw_identity()) };
    }

    /// Calls `callback` with the qualified name and the value of each attribute in the element's attribute list, in
    /// order.
    pub(crate) fn for_each_attribute(&self, mut callback: impl FnMut(&Utf16FlyString, &Utf16String)) {
        let mut callback: &mut dyn FnMut(&Utf16FlyString, &Utf16String) = &mut callback;
        // SAFETY: The element is live, and the callback outlives the call.
        unsafe { web_dom_element_for_each_attribute(self.0.as_ptr(), (&raw mut callback).cast(), on_attribute) };
    }

    /// https://dom.spec.whatwg.org/#concept-element-attributes-get-value
    pub(crate) fn attribute(&self, name: &str) -> Option<Utf16String> {
        let mut value = None;
        // SAFETY: The element is live, the name is lent for the call, and the value outlives it.
        unsafe {
            web_dom_element_attribute(
                self.0.as_ptr(),
                ak::Span::new(name.as_bytes()),
                (&raw mut value).cast(),
                on_value,
            );
        }
        value
    }
}

// https://dom.spec.whatwg.org/#valid-attribute-local-name
pub(crate) fn is_valid_attribute_local_name(name: &Utf16String) -> bool {
    // SAFETY: The name is live for the call.
    unsafe { web_dom_is_valid_attribute_local_name(name.raw_identity()) }
}

/// https://drafts.csswg.org/web-animations-1/#dom-animatable-getanimations
pub(crate) fn get_animations<'vm>(
    this: &CppInterface,
    cx: &mut CallContext<'vm>,
    options: GetAnimationsOptions,
) -> WebIdlResult<MarkedVec<'vm, AnyAnimation>> {
    animatable::get_animations(this, cx, options)
}

/// https://drafts.csswg.org/web-animations-1/#dom-animatable-animate
pub(crate) fn animate(
    this: &CppInterface,
    cx: &mut CallContext,
    keyframes: Option<Gc<Object>>,
    options: UnrestrictedDoubleOrKeyframeAnimationOptions,
) -> WebIdlResult<AnyAnimation> {
    animatable::animate(this, cx, keyframes, options)
}
