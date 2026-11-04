/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the interfaces that inherit from CSSStyleDeclaration, which LibWeb implements in C++, share: each owns a
//! C++ declaration block and has an attribute for each of its properties or descriptors.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Interface, Ref, cell_into_abi};
use crate::bindings::{Exception, WebIdlResult};

unsafe extern "C" {
    fn web_css_descriptors_set_owner(descriptors: *mut c_void, interface_name: u16, owner: *mut c_void);
    fn web_css_descriptors_get(descriptors: *mut c_void, name: *const u8, name_length: usize) -> usize;
    fn web_css_descriptors_set(
        descriptors: *mut c_void,
        name: *const u8,
        name_length: usize,
        value: usize,
        error_message: *mut usize,
    ) -> bool;
}

/// The Web::CSS::RustCSSDescriptors that an object of a descriptors interface owns.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppDescriptors(CppCell);

impl CppDescriptors {
    pub(crate) fn new(cell: CppCell) -> Self {
        Self(cell)
    }

    pub(crate) fn as_cpp_parent(&self) -> Option<NonNull<c_void>> {
        NonNull::new(self.0.as_ptr())
    }

    pub(crate) fn set_owner<T: Interface>(&self, owner: Ref<T>) {
        // SAFETY: The cells are live, and the owner keeps the descriptors alive.
        unsafe { web_css_descriptors_set_owner(self.0.as_ptr(), T::INTERFACE_NAME, cell_into_abi(owner)) };
    }

    pub(crate) fn get(&self, name: &str) -> Utf16String {
        // SAFETY: The cell is live, the name is lent for the call, and LibWeb hands over the string.
        unsafe { Utf16String::from_raw_owned(web_css_descriptors_get(self.0.as_ptr(), name.as_ptr(), name.len())) }
    }

    pub(crate) fn set(&self, name: &str, value: Utf16String) -> WebIdlResult<()> {
        let mut error_message = 0;
        // SAFETY: The cell is live, the name is lent for the call, and the value is handed over.
        if unsafe {
            web_css_descriptors_set(
                self.0.as_ptr(),
                name.as_ptr(),
                name.len(),
                value.into_raw(),
                &raw mut error_message,
            )
        } {
            return Ok(());
        }
        // SAFETY: LibWeb hands over the message of the exception.
        let message = unsafe { Utf16String::from_raw_owned(error_message) };
        Err(Exception::Dom("NoModificationAllowedError", message))
    }
}

/// An interface of the descriptors of an at-rule, with the entry point through which LibWeb creates its objects, and
/// the getter and the setter of each descriptor.
macro_rules! descriptors_interface {
    ($interface:ident, $create:ident, $(($getter:ident, $setter:ident, $name:literal)),+ $(,)?) => {
        #[derive(libjs_rust::api::Trace)]
        #[gc(crate = libjs_rust)]
        pub(crate) struct $interface {
            descriptors: $crate::css::css_descriptors::CppDescriptors,
        }

        impl $crate::bindings::interface::Wrappable for $interface {
            fn cpp_parent(&self) -> Option<core::ptr::NonNull<core::ffi::c_void>> {
                self.descriptors.as_cpp_parent()
            }
        }

        impl $interface {
            $(
                pub(crate) fn $getter(
                    &self,
                    _: &mut $crate::bindings::CallContext,
                ) -> $crate::bindings::WebIdlResult<ak::Utf16String> {
                    Ok(self.descriptors.get($name))
                }

                pub(crate) fn $setter(
                    &self,
                    _: &mut $crate::bindings::CallContext,
                    value: ak::Utf16String,
                ) -> $crate::bindings::WebIdlResult<()> {
                    self.descriptors.set($name, value)
                }
            )+
        }

        /// A new object that owns the descriptors, which LibWeb created.
        ///
        /// # Safety
        ///
        /// `vm` must be the live VM, and `descriptors` the live cell of a Web::CSS::RustCSSDescriptors.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $create(vm: *mut core::ffi::c_void, descriptors: *mut core::ffi::c_void) {
            assert!(!vm.is_null(), "C++ passes the VM");
            // SAFETY: The caller passes the live VM and a live cell.
            let (vm, descriptors) = unsafe {
                (&*vm.cast::<libjs_rust::api::Vm>(), $crate::bindings::cpp_cell::CppCell::from_raw(descriptors))
            };
            let object = $crate::bindings::interface::allocate(vm, |_| $interface {
                descriptors: $crate::css::css_descriptors::CppDescriptors::new(descriptors),
            });
            object.descriptors.set_owner(object);
        }
    };
}

pub(crate) use descriptors_interface;
