/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{ForeignCellSlot, Gc, GcWeak, Trace, Vm};

/// A cell of LibWeb's C++ code, which this keeps alive.
#[derive(Trace)]
pub(crate) struct CppCell(ForeignCellSlot);

impl Clone for CppCell {
    fn clone(&self) -> Self {
        // SAFETY: The slot keeps the cell alive.
        unsafe { Self::from_raw(self.0.as_ptr()) }
    }
}

/// The C++ classes of the cells that LibWeb's C++ code lends the exports of the crate, which name the class of a
/// LentCppCell.
pub(crate) mod cpp_class {
    /// A GC::Cell of any class, which the code that the export hands it to does not rely on.
    pub(crate) enum Cell {}

    /// A GC::Function<void()> that runs an abort algorithm.
    pub(crate) enum AbortAlgorithm {}
    /// A GC::Function<void(ByteBuffer)>, a Streams::ReadableStreamDefaultReader::SuccessSteps.
    pub(crate) enum BytesSteps {}
    /// A CSS::CSSRule.
    pub(crate) enum CSSRule {}
    /// A CSS::CSSStyleDeclaration.
    pub(crate) enum CSSStyleDeclaration {}
    /// A CSS::CSSStyleProperties.
    pub(crate) enum CSSStyleProperties {}
    /// A CSS::CSSStyleSheet.
    pub(crate) enum CSSStyleSheet {}
    /// A DOM::Document.
    pub(crate) enum Document {}
    /// A DOM::Element.
    pub(crate) enum Element {}
    /// An HTML::EnvironmentSettingsObject.
    pub(crate) enum EnvironmentSettingsObject {}
    /// A DOM::Event.
    pub(crate) enum Event {}
    /// An HTML::EventLoop.
    pub(crate) enum EventLoop {}
    /// A DOM::EventTarget.
    pub(crate) enum EventTarget {}
    /// A Fetch::Infrastructure::FetchController.
    pub(crate) enum FetchController {}
    /// A DOM::Node.
    pub(crate) enum Node {}
    /// A GC::Function<GC::Ref<WebIDL::Promise>()>, a Streams::PullAlgorithm or a Streams::FlushAlgorithm.
    pub(crate) enum PromiseAlgorithm {}
    /// A GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>, a Streams::CancelAlgorithm, a Streams::TransformAlgorithm
    /// or a Streams::WriteAlgorithm.
    pub(crate) enum PromiseAlgorithmWithArgument {}
    /// A Streams::ReadRequest.
    pub(crate) enum ReadRequest {}
    /// A Fetch::Infrastructure::ResponseView.
    pub(crate) enum ResponseView {}
    /// A CSS::StyleSheetImportCell.
    pub(crate) enum StyleSheetImportCell {}
    /// A CSS::StyleSheetStateCell.
    pub(crate) enum StyleSheetStateCell {}
    /// An HTML::TextTrack.
    pub(crate) enum TextTrack {}
    /// A GC::Function<void(JS::Value)>, a Streams::ReadableStreamDefaultReader::FailureSteps.
    pub(crate) enum ValueSteps {}
    /// An HTML::Window.
    pub(crate) enum Window {}
}

/// A cell of LibWeb's C++ code, of the C++ class that `Class` names, which C++ lends the exports of the crate for as
/// long as they run. The exports take a `&LentCppCell<Class>` where C++ passes such a cell, as they take a `&Vm` where
/// it passes the VM, so a reference to one is always a live cell of that class. The wrappers of the cells of a class
/// make themselves only of a cell lent as one of that class; CppCell, which assumes no class, of any.
#[repr(C)]
pub(crate) struct LentCppCell<Class> {
    // NB: The cell is mutable behind a shared reference, and bigger than Rust sees, so a reference to one must not tell
    //     the compiler that the memory behind it is read only or unaliased.
    _opaque: core::cell::UnsafeCell<[u8; 0]>,
    _not_send_sync_or_unpin: core::marker::PhantomData<(*const (), core::marker::PhantomPinned)>,
    _class: core::marker::PhantomData<Class>,
}

impl CppCell {
    /// Keeps alive the cell that C++ lends, whatever its class.
    pub(crate) fn from_lent<Class>(cell: &LentCppCell<Class>) -> Self {
        // SAFETY: A LentCppCell is a live cell.
        unsafe { Self::from_raw(NonNull::from(cell).as_ptr().cast()) }
    }

    /// # Safety
    ///
    /// `cell` must be a live cell of LibWeb's C++ code.
    pub(crate) unsafe fn from_raw(cell: *mut c_void) -> Self {
        let slot = ForeignCellSlot::empty();
        // SAFETY: The caller passes a live cell.
        unsafe { slot.set(Some(NonNull::new(cell).expect("C++ passes a cell"))) };
        Self(slot)
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }
}

/// A cell of LibWeb's C++ code, which this does not keep alive.
#[derive(Clone, Trace)]
pub(crate) struct WeakCppCell(GcWeak<c_void>);

impl WeakCppCell {
    pub(crate) fn new(vm: &Vm, cell: &CppCell) -> Self {
        let cell = NonNull::new(cell.as_ptr()).expect("a CppCell holds a cell");
        // SAFETY: The CppCell keeps the cell alive.
        Self(GcWeak::new(vm.heap(), unsafe { Gc::from_non_null(cell) }))
    }

    pub(crate) fn get(&self) -> Option<CppCell> {
        // SAFETY: A cell that the weak reference still has is live.
        self.0.get().map(|cell| unsafe { CppCell::from_raw(cell.as_ptr()) })
    }
}
