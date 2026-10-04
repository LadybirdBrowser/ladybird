/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Host arrays, of kind JS_HOST_CLASS_ARRAY.

use libjs_runtime_macros::Trace;

use crate::gc::foreign::ForeignCellSlot;
use crate::layout::host_class::JSHostClass;
use crate::runtime::array::Array;

/// An Array exotic object whose [[Set]] and [[Delete]] may come from its host class, with a C++ GC cell for any state
/// of its own.
#[repr(C)]
#[derive(Trace)]
pub struct HostArray {
    pub base: Array,
    #[gc(untraced)]
    pub host_class: &'static JSHostClass,
    pub host_data: ForeignCellSlot,
}
