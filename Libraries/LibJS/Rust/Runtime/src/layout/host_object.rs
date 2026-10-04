/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cell of a host object of kind JS_HOST_CLASS_OBJECT, whose layout LibJS/HostObjectABI.h fixes for every
//! runtime.

use core::mem::offset_of;

use super::host_class::JSHostClass;
use super::object::Object;
use crate::layout_forward::ForeignCellSlot;

pub const JS_HOST_OBJECT_HOST_CLASS_OFFSET: usize = 72;
pub const JS_HOST_OBJECT_WRAPPABLE_OFFSET: usize = 80;
pub const JS_HOST_OBJECT_HOST_DATA_OFFSET: usize = 88;
pub const JS_HOST_OBJECT_SIZE: usize = 96;

#[repr(C)]
pub struct HostObject {
    pub base: Object,
    pub host_class: &'static JSHostClass,
    /// The embedder's wrapped implementation object, a C++ GC cell, or null. Direct getter functions read this slot
    /// without checking what it holds, so it never holds anything else.
    pub wrappable: ForeignCellSlot,
    /// A C++ GC cell with the rest of the embedder's per-object state, or null.
    pub host_data: ForeignCellSlot,
}

const _: () = assert!(offset_of!(HostObject, host_class) == JS_HOST_OBJECT_HOST_CLASS_OFFSET);
const _: () = assert!(offset_of!(HostObject, wrappable) == JS_HOST_OBJECT_WRAPPABLE_OFFSET);
const _: () = assert!(offset_of!(HostObject, host_data) == JS_HOST_OBJECT_HOST_DATA_OFFSET);
const _: () = assert!(size_of::<HostObject>() == JS_HOST_OBJECT_SIZE);
