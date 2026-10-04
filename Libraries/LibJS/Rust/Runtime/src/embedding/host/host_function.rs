/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Host functions, of kind JS_HOST_CLASS_FUNCTION.

use libjs_runtime_macros::Trace;

use crate::gc::foreign::ForeignCellSlot;
use crate::layout::host_class::JSHostClass;
use crate::runtime::native_function::NativeFunction;

/// A built-in function whose [[Call]] and [[Construct]] come from its host class, with a C++ GC cell for any state
/// of its own.
#[repr(C)]
#[derive(Trace)]
pub struct HostFunction {
    pub base: NativeFunction,
    #[gc(untraced)]
    pub host_class: &'static JSHostClass,
    pub host_data: ForeignCellSlot,
}
