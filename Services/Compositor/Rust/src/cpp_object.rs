/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A C++ object that the core owns, such as a window server or a remote layer, which it only hands back to the C++ that
//! made it and destroys with the function that came with it.

use std::ffi::c_void;
use std::ptr::NonNull;

/// A C++ object as the C++ side hands it over, or null for none.
#[repr(C)]
pub struct FfiCppObject {
    pub object: *mut c_void,
    pub destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

impl Default for FfiCppObject {
    fn default() -> Self {
        Self {
            object: std::ptr::null_mut(),
            destroy: None,
        }
    }
}

pub struct CppObject {
    object: NonNull<c_void>,
    destroy: unsafe extern "C" fn(*mut c_void),
}

impl CppObject {
    /// # Safety
    ///
    /// A non-null object must be live and handed over, and its destroy function must destroy it.
    pub unsafe fn adopt(object: FfiCppObject) -> Option<CppObject> {
        Some(CppObject {
            object: NonNull::new(object.object)?,
            destroy: object.destroy.expect("a C++ object comes with its destroy function"),
        })
    }

    pub fn as_ptr(&self) -> *mut c_void {
        self.object.as_ptr()
    }
}

impl Drop for CppObject {
    fn drop(&mut self) {
        // SAFETY: This owns the object.
        unsafe { (self.destroy)(self.object.as_ptr()) };
    }
}

#[cfg(test)]
pub mod testing {
    use super::*;

    unsafe extern "C" fn destroy_nothing(_: *mut c_void) {}

    /// An object that stands for one of the C++ side, which is told apart by its address.
    pub fn object_for_testing(address: usize) -> CppObject {
        CppObject {
            object: NonNull::new(address as *mut c_void).expect("an address"),
            destroy: destroy_nothing,
        }
    }
}
