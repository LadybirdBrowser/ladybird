/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The libjs_rust items the lints know by path, at the same paths, and the types they find through its marker traits.

pub mod layout {
    pub mod cell {
        pub struct Gc<T: ?Sized>(core::ptr::NonNull<T>);

        impl<T: ?Sized> Clone for Gc<T> {
            fn clone(&self) -> Self {
                *self
            }
        }

        impl<T: ?Sized> Copy for Gc<T> {}

        impl<T: crate::gc::class::GcCell> core::ops::Deref for Gc<T> {
            type Target = T;

            fn deref(&self) -> &T {
                // SAFETY: Stand-in.
                unsafe { self.0.as_ref() }
            }
        }
    }

    pub mod value {
        #[derive(Clone, Copy)]
        pub struct Value(pub u64);
    }
}

pub mod gc {
    pub mod visitor {
        pub struct Visitor;

        /// # Safety
        ///
        /// Stand-in.
        pub unsafe trait Trace {
            fn trace(&self, visitor: &mut Visitor);
        }

        unsafe impl<T> Trace for crate::layout::cell::Gc<T> {
            fn trace(&self, _: &mut Visitor) {}
        }

        unsafe impl Trace for crate::layout::value::Value {
            fn trace(&self, _: &mut Visitor) {}
        }

        unsafe impl<T: Trace> Trace for Vec<T> {
            fn trace(&self, _: &mut Visitor) {}
        }
    }

    pub mod class {
        /// # Safety
        ///
        /// Stand-in.
        pub unsafe trait GcCell: super::visitor::Trace {}
    }

    pub mod root {
        use super::visitor::Trace;

        pub struct Vm {
            pub roots: Vec<crate::layout::value::Value>,
        }

        pub struct Root<'vm, T: Trace + 'static> {
            pub vm: &'vm Vm,
            pub value: Box<T>,
        }

        pub struct MarkedVec<'vm, T: Trace + Clone + 'static> {
            pub vm: &'vm Vm,
            pub values: core::cell::OnceCell<Root<'vm, Vec<T>>>,
        }

        impl<'vm, T: Trace + Clone + 'static> MarkedVec<'vm, T> {
            pub fn new(vm: &'vm Vm) -> Self {
                Self {
                    vm,
                    values: core::cell::OnceCell::new(),
                }
            }
        }
    }

    pub mod defer_gc {
        pub struct DeferGc<'vm>(pub &'vm super::root::Vm);

        impl Drop for DeferGc<'_> {
            fn drop(&mut self) {}
        }

        impl DeferGc<'_> {
            pub fn cover<T>(&self, value: T) -> Deferred<'_, T> {
                Deferred(value, core::marker::PhantomData)
            }
        }

        pub struct Deferred<'d, T>(pub T, pub core::marker::PhantomData<&'d ()>);
    }

    pub mod lint_markers {
        /// # Safety
        ///
        /// Stand-in.
        pub unsafe trait KeepsAlive {}

        unsafe impl<T: super::visitor::Trace + 'static> KeepsAlive for super::root::Root<'_, T> {}
        unsafe impl<T: super::visitor::Trace + Clone + 'static> KeepsAlive for super::root::MarkedVec<'_, T> {}
        unsafe impl<T> KeepsAlive for super::defer_gc::Deferred<'_, T> {}
        unsafe impl KeepsAlive for super::root::Vm {}

        /// # Safety
        ///
        /// Stand-in.
        pub unsafe trait DefersCollections {}

        unsafe impl DefersCollections for super::defer_gc::DeferGc<'_> {}

        unsafe impl<T> DefersCollections for super::defer_gc::Deferred<'_, T> {}

        /// # Safety
        ///
        /// Stand-in.
        pub unsafe trait HoldsNoCells {}
    }

    pub mod weak {
        /// # Safety
        ///
        /// Stand-in.
        pub unsafe trait SweptAfterCollection {}

        pub struct GcWeak<T> {
            pub slot: usize,
            pub cell: core::marker::PhantomData<fn() -> crate::layout::cell::Gc<T>>,
        }
    }
}

pub use gc::defer_gc::{DeferGc, Deferred};
pub use gc::root::{MarkedVec, Root, Vm};
pub use gc::visitor::{Trace, Visitor};
pub use gc::weak::GcWeak;
pub use layout::cell::Gc;
pub use layout::value::Value;

pub struct Cell;

unsafe impl Trace for Cell {
    fn trace(&self, _: &mut Visitor) {}
}

unsafe impl gc::class::GcCell for Cell {}

impl Cell {
    pub fn method(&self) {}
}
