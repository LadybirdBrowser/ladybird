/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates and reads the ImageData and ImageBitmap objects that it handles,
//! which Libraries/LibWeb/HTML/ImageData.cpp and Libraries/LibWeb/HTML/ImageBitmap.cpp declare. A cell is the cell of
//! an object of the interface that the name of the function says.

use core::ffi::c_void;
use core::ptr::NonNull;

use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{RawNativeFunctionResult, Value, Vm};

use super::image_bitmap::ImageBitmap;
use super::image_data::ImageData;
use crate::bindings::CallContext;
use crate::bindings::interface::{Interface, Ref, cell_from_abi, cell_into_abi, realm_arguments};

/// # Safety
///
/// `vm` must be the live VM.
unsafe fn vm_from_abi<'vm>(vm: *mut c_void) -> &'vm Vm {
    assert!(!vm.is_null(), "C++ passes the VM");
    // SAFETY: The caller passes the live VM.
    unsafe { &*vm.cast::<Vm>() }
}

/// The implementation of the object whose cell C++ lends, for as long as C++ keeps the cell alive.
///
/// # Safety
///
/// `cell` must be the live cell of an object of the interface, and it must outlive `'a`.
unsafe fn object_from_abi<'a, T: Interface>(cell: *mut c_void) -> &'a T {
    // SAFETY: The caller passes a live cell of the interface.
    let object: Ref<T> = unsafe { cell_from_abi(cell) };
    // SAFETY: The caller keeps the cell alive for 'a.
    unsafe { &*object.as_ptr() }
}

/// Creates an ImageData of the size, with transparent black pixels and the sRGB color space, in the realm, and stores
/// its cell, or throws what initializing an ImageData throws.
///
/// # Safety
///
/// `vm` must be the live VM, and `realm` a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_create(
    vm: *mut c_void,
    realm: *mut c_void,
    width: u32,
    height: u32,
    out_image_data: &mut *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let mut cx = CallContext::new(vm, realm);
    let result = ImageData::create(&mut cx, width, height, None, None).map(|image_data| {
        *out_image_data = cell_into_abi(image_data);
        Value::UNDEFINED
    });
    RawNativeFunctionResult::from_completion(result.map_err(|exception| exception.throw(&cx)))
}

/// # Safety
///
/// `image_data` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_width(image_data: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageData>(image_data) }.size().0
}

/// # Safety
///
/// `image_data` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_height(image_data: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageData>(image_data) }.size().1
}

/// # Safety
///
/// `image_data` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_is_detached(image_data: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageData>(image_data) }.is_detached()
}

/// The first byte of the pixels of the ImageData, or null if its buffer is detached or its data has no room for them.
///
/// # Safety
///
/// `image_data` must be a live cell. The pixels are valid until script runs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_pixels(image_data: *mut c_void) -> *mut u8 {
    // SAFETY: The caller passes a live cell.
    let image_data = unsafe { object_from_abi::<ImageData>(image_data) };
    image_data.pixels().map_or(core::ptr::null_mut(), NonNull::as_ptr)
}

/// `frame` is a Gfx::DecodedImageFrame that the caller lends for the call, which the ImageBitmap keeps a copy of as its
/// bitmap data, or null for none.
///
/// # Safety
///
/// `vm` must be the live VM, and `frame` a live frame or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_create(vm: *mut c_void, frame: *const c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live frame.
    let frame = (!frame.is_null()).then(|| unsafe { ImageFrameHandle::retain(frame) });
    cell_into_abi(ImageBitmap::create(vm, frame))
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_width(image_bitmap: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.width_of_bitmap()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_height(image_bitmap: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.height_of_bitmap()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_is_detached(image_bitmap: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.is_detached()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_close(image_bitmap: *mut c_void) {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.close_bitmap();
}

/// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
///
/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_origin_clean(image_bitmap: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.origin_clean()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_set_origin_clean(image_bitmap: *mut c_void, origin_clean: bool) {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.set_origin_clean(origin_clean);
}

/// The Gfx::DecodedImageFrame of the bitmap data of the ImageBitmap, or null if it has none.
///
/// # Safety
///
/// `image_bitmap` must be a live cell. The frame is valid until the bitmap data is unset, by close() or a transfer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_frame(image_bitmap: *mut c_void) -> *const c_void {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.raw_frame()
}
