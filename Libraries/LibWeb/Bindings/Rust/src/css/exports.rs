/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the C++ code of LibWeb asks of the interfaces of CSS, declared in Libraries/LibWeb/CSS/MediaList.cpp.

use core::ffi::c_void;

use libjs_rust::api::Vm;
use libweb_rust::NativeMediaList;

use super::media_list::MediaList;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, allocate, cell_from_abi, cell_into_abi};

/// # Safety
///
/// `vm` must be the live VM.
unsafe fn vm_from_abi<'vm>(vm: *mut c_void) -> &'vm Vm {
    assert!(!vm.is_null(), "C++ passes the VM");
    // SAFETY: The caller passes the live VM.
    unsafe { &*vm.cast::<Vm>() }
}

/// The implementation of the MediaList whose cell C++ lends, for as long as C++ keeps the cell alive.
///
/// # Safety
///
/// `list` must be the live cell of a MediaList, and it must outlive `'a`.
unsafe fn media_list_from_abi<'a>(list: *mut c_void) -> &'a MediaList {
    // SAFETY: The caller passes a live cell of a MediaList.
    let list: Ref<MediaList> = unsafe { cell_from_abi(list) };
    // SAFETY: The caller keeps the cell alive for 'a.
    unsafe { &*list.as_ptr() }
}

/// A new MediaList that shares the media queries of `media`.
///
/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_list_create(vm: *mut c_void, media: &NativeMediaList) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    cell_into_abi(allocate(vm, |defer_gc| MediaList::new(defer_gc, media.clone())))
}

/// # Safety
///
/// `vm` must be the live VM, `list` the live cell of a MediaList, and `sheet` that of a CSSStyleSheet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_list_set_associated_style_sheet(
    vm: *mut c_void,
    list: *mut c_void,
    sheet: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, and live cells of a MediaList and a CSSStyleSheet.
    let (vm, list, sheet) = unsafe { (vm_from_abi(vm), media_list_from_abi(list), CppCell::from_raw(sheet)) };
    list.set_associated_style_sheet(vm, sheet);
}

/// # Safety
///
/// `vm` must be the live VM, `list` the live cell of a MediaList, and `rule` that of a CSSRule.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_list_set_associated_rule(vm: *mut c_void, list: *mut c_void, rule: *mut c_void) {
    // SAFETY: The caller passes the live VM, and live cells of a MediaList and a CSSRule.
    let (vm, list, rule) = unsafe { (vm_from_abi(vm), media_list_from_abi(list), CppCell::from_raw(rule)) };
    list.set_associated_rule(vm, rule);
}

/// A native list of its own with the media queries of the MediaList as they are now, which the caller frees.
///
/// # Safety
///
/// `list` must be the live cell of a MediaList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_list_share_native_list(list: *mut c_void) -> *mut NativeMediaList {
    // SAFETY: The caller passes a live cell of a MediaList, and keeps it alive for the call.
    let media = unsafe { media_list_from_abi(list) }.share_media();
    Box::into_raw(Box::new(media))
}
