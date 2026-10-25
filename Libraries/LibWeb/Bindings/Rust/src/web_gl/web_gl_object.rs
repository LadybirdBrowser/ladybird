/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.3

use core::cell::{Cell, RefCell};

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::state::{ContextRef, ContextState};
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, WebIdlResult};

pub(crate) struct InvalidOperation;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLObject {
    context: ContextRef,
    #[gc(untraced)]
    handle: u32,
    /// Restoring a lost context starts a new generation, in which the objects of earlier ones are invalid.
    #[gc(untraced)]
    context_generation: u64,
    #[gc(untraced)]
    invalidated: Cell<bool>,
    #[gc(untraced)]
    label: RefCell<UsvString>,
}

impl Wrappable for WebGLObject {
    fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
        self.context.state().canvas().relevant_realm()
    }
}

impl WebGLObject {
    pub(crate) fn new(context: &ContextState, handle: u32) -> Self {
        Self {
            context: context.context_ref(),
            handle,
            context_generation: context.context_generation(),
            invalidated: Cell::new(false),
            label: RefCell::new(UsvString::from_ascii(b"")),
        }
    }

    pub(crate) fn label(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.label.borrow().clone())
    }

    pub(crate) fn set_label(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        self.label.replace(value);
        Ok(())
    }

    fn validate_context(&self, context: &ContextState) -> Result<(), InvalidOperation> {
        if core::ptr::eq(self.context.state(), context) {
            Ok(())
        } else {
            Err(InvalidOperation)
        }
    }

    pub(crate) fn invalidated_for_context(&self, context: &ContextState) -> bool {
        self.invalidated.get() || self.context_generation != context.context_generation()
    }

    pub(crate) fn invalidate(&self) {
        self.invalidated.set(true);
    }

    pub(crate) fn handle(&self, context: &ContextState) -> Result<u32, InvalidOperation> {
        self.handle_for_query(context)?.ok_or(InvalidOperation)
    }

    pub(crate) fn handle_for_deletion(&self, context: &ContextState) -> Result<Option<u32>, InvalidOperation> {
        let handle = self.handle_for_query(context)?;
        if handle.is_some() {
            self.invalidate();
        }
        Ok(handle)
    }

    /// `None` for a deleted or lost object, which a question about it has an answer for without asking GL.
    pub(crate) fn handle_for_query(&self, context: &ContextState) -> Result<Option<u32>, InvalidOperation> {
        self.validate_context(context)?;
        Ok((!self.invalidated_for_context(context)).then_some(self.handle))
    }
}

macro_rules! modules_of_interfaces_that_only_inherit_from_web_gl_object {
    ($(
        $(#[$specification_link:meta])*
        $module:ident :: $interface:ident
    ),+ $(,)?) => {$(
        $(#[$specification_link])*
        pub(crate) mod $module {
            use libjs_rust::api::{Gc, Realm, Trace, Vm};

            use $crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
            use $crate::web_gl::state::ContextState;
            use $crate::web_gl::web_gl_object::WebGLObject;

            #[derive(Trace)]
            #[gc(crate = libjs_rust)]
            pub(crate) struct $interface {
                object: WebGLObject,
            }

            impl Wrappable for $interface {
                fn relevant_realm(&self, vm: &Vm) -> Option<Gc<Realm>> {
                    self.object.relevant_realm(vm)
                }
            }

            impl Inherits<WebGLObject> for $interface {
                fn parent(&self) -> &WebGLObject {
                    &self.object
                }
            }

            impl $interface {
                pub(crate) fn create(vm: &Vm, context: &ContextState, handle: u32) -> Ref<Self> {
                    allocate(
                        vm,
                        |_| Self {
                            object: WebGLObject::new(context, handle),
                        },
                    )
                }
            }
        }
    )+};
}
pub(crate) use modules_of_interfaces_that_only_inherit_from_web_gl_object;
