/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::{MarkedVec, Vm};

use crate::bindings::conversions::CppInterface;
use crate::dom::element::CppElement;

unsafe extern "C" {
    fn web_html_form_element_submittable_elements(
        form: *mut c_void,
        elements: *mut c_void,
        on_element: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
}

unsafe extern "C" fn on_element(elements: *mut c_void, element: *mut c_void) {
    // SAFETY: elements is the list of the caller, and LibWeb passes a live Element.
    unsafe { (*elements.cast::<MarkedVec<'_, CppElement>>()).push(CppElement::from_raw(element)) };
}

/// https://html.spec.whatwg.org/multipage/forms.html#the-form-element
#[derive(Clone, Copy)]
pub(crate) struct CppHTMLFormElement(CppInterface);

impl CppHTMLFormElement {
    /// # Safety
    ///
    /// `form` must be the wrapper of an HTMLFormElement.
    pub(crate) unsafe fn from_interface(form: CppInterface) -> Self {
        Self(form)
    }

    pub(crate) fn interface(self) -> CppInterface {
        self.0
    }

    /// The submittable elements whose form owner is the form, in tree order.
    pub(crate) fn submittable_elements<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, CppElement> {
        let elements = MarkedVec::new(vm);
        // SAFETY: The form is live, and the list outlives the call.
        unsafe {
            web_html_form_element_submittable_elements(
                self.0.wrapper().as_ptr().cast(),
                (&raw const elements).cast_mut().cast(),
                on_element,
            );
        }
        elements
    }
}
