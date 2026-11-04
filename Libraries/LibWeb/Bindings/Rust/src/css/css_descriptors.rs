/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the interfaces of the descriptors of an at-rule share: each is a CSSStyleDeclaration with an attribute for each
//! descriptor.

/// An interface of the descriptors of an at-rule, with the entry point through which LibWeb creates its objects, and
/// the getter and the setter of each descriptor.
macro_rules! descriptors_interface {
    ($interface:ident, $create:ident, $(($getter:ident, $setter:ident, $name:literal)),+ $(,)?) => {
        #[derive(libjs_rust::api::Trace)]
        #[gc(crate = libjs_rust)]
        pub(crate) struct $interface {
            declaration: $crate::css::css_style_declaration::CSSStyleDeclaration,
        }

        impl $crate::bindings::interface::Wrappable for $interface {
            fn cpp_parent(&self) -> Option<core::ptr::NonNull<core::ffi::c_void>> {
                self.declaration.as_cpp_parent()
            }
        }

        impl $crate::bindings::interface::Inherits<$crate::css::css_style_declaration::CSSStyleDeclaration>
            for $interface
        {
            fn parent(&self) -> &$crate::css::css_style_declaration::CSSStyleDeclaration {
                &self.declaration
            }
        }

        impl $interface {
            $(
                pub(crate) fn $getter(
                    &self,
                    _: &mut $crate::bindings::CallContext,
                ) -> $crate::bindings::WebIdlResult<ak::Utf16String> {
                    Ok(self.declaration.get($name))
                }

                pub(crate) fn $setter(
                    &self,
                    _: &mut $crate::bindings::CallContext,
                    value: ak::Utf16String,
                ) -> $crate::bindings::WebIdlResult<()> {
                    self.declaration.set($name, value)
                }
            )+
        }

        /// A new object that owns the descriptors, which LibWeb created.
        ///
        /// # Safety
        ///
        /// `vm` must be the live VM, and `descriptors` the live cell of a Web::CSS::CSSDescriptors.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $create(vm: *mut core::ffi::c_void, descriptors: *mut core::ffi::c_void) {
            assert!(!vm.is_null(), "C++ passes the VM");
            // SAFETY: The caller passes the live VM and a live cell.
            let (vm, descriptors) = unsafe {
                (&*vm.cast::<libjs_rust::api::Vm>(), $crate::bindings::cpp_cell::CppCell::from_raw(descriptors))
            };
            let object = $crate::bindings::interface::allocate(vm, |_| $interface {
                declaration: $crate::css::css_style_declaration::CSSStyleDeclaration::new(descriptors),
            });
            object.declaration.set_owner(object);
        }
    };
}

pub(crate) use descriptors_interface;
