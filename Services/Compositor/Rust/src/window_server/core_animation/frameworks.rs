/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C functions and constants of the frameworks below CoreAnimation that the window server is driven through:
//! CoreFoundation, CoreGraphics, IOSurface, CoreVideo and CoreMedia. Only what is called is declared.

use std::ffi::{CStr, c_char, c_void};
use std::ptr::NonNull;

pub type CFTypeRef = *const c_void;
pub type CFStringRef = *const c_void;
pub type CFArrayRef = *const c_void;
pub type CFDictionaryRef = *const c_void;
pub type CFMutableDictionaryRef = *mut c_void;
pub type CFDataRef = *const c_void;
pub type CFNumberRef = *const c_void;
pub type CFIndex = isize;
pub type CGFloat = f64;
pub type CGPathRef = *const c_void;
pub type CGMutablePathRef = *mut c_void;
pub type CGColorRef = *const c_void;
pub type IOSurfaceRef = *mut c_void;
pub type CVPixelBufferRef = *mut c_void;
pub type CMVideoFormatDescriptionRef = *const c_void;
pub type CMSampleBufferRef = *const c_void;
pub type OSStatus = i32;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct CGPoint {
    pub x: CGFloat,
    pub y: CGFloat,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct CGSize {
    pub width: CGFloat,
    pub height: CGFloat,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct CGRect {
    pub origin: CGPoint,
    pub size: CGSize,
}

impl CGRect {
    pub fn new(x: CGFloat, y: CGFloat, width: CGFloat, height: CGFloat) -> Self {
        Self {
            origin: CGPoint { x, y },
            size: CGSize { width, height },
        }
    }
}

/// Maps a point to `(a x + c y + tx, b x + d y + ty)`.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CGAffineTransform {
    pub a: CGFloat,
    pub b: CGFloat,
    pub c: CGFloat,
    pub d: CGFloat,
    pub tx: CGFloat,
    pub ty: CGFloat,
}

impl CGAffineTransform {
    pub const fn new(a: CGFloat, b: CGFloat, c: CGFloat, d: CGFloat, tx: CGFloat, ty: CGFloat) -> Self {
        Self { a, b, c, d, tx, ty }
    }

    pub const fn scale(x: CGFloat, y: CGFloat) -> Self {
        Self::new(x, 0.0, 0.0, y, 0.0, 0.0)
    }

    pub const fn translation(x: CGFloat, y: CGFloat) -> Self {
        Self::new(1.0, 0.0, 0.0, 1.0, x, y)
    }

    /// This transform, and then `then`, as `CGAffineTransformConcat(self, then)` does.
    pub fn then(self, then: Self) -> Self {
        Self {
            a: self.a * then.a + self.b * then.c,
            b: self.a * then.b + self.b * then.d,
            c: self.c * then.a + self.d * then.c,
            d: self.c * then.b + self.d * then.d,
            tx: self.tx * then.a + self.ty * then.c + then.tx,
            ty: self.tx * then.b + self.ty * then.d + then.ty,
        }
    }
}

/// A time of CoreMedia.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CMTime {
    pub value: i64,
    pub timescale: i32,
    pub flags: u32,
    pub epoch: i64,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CMSampleTimingInfo {
    pub duration: CMTime,
    pub presentation_time_stamp: CMTime,
    pub decode_time_stamp: CMTime,
}

#[repr(C)]
pub struct CFArrayCallBacks {
    _private: [u8; 0],
}

#[repr(C)]
pub struct CFDictionaryCallBacks {
    _private: [u8; 0],
}

// kCFNumberDoubleType of CFNumber.h.
pub const CF_NUMBER_DOUBLE_TYPE: CFIndex = 13;
// kCFStringEncodingUTF8 of CFString.h.
const CF_STRING_ENCODING_UTF8: u32 = 0x0800_0100;
// kCVAttachmentMode_ShouldPropagate of CVBuffer.h.
pub const CV_ATTACHMENT_MODE_SHOULD_PROPAGATE: u32 = 1;
pub const CV_RETURN_SUCCESS: i32 = 0;

#[link(name = "CoreFoundation", kind = "framework")]
unsafe extern "C" {
    pub static kCFTypeArrayCallBacks: CFArrayCallBacks;
    pub static kCFTypeDictionaryKeyCallBacks: CFDictionaryCallBacks;
    pub static kCFTypeDictionaryValueCallBacks: CFDictionaryCallBacks;
    pub static kCFBooleanTrue: CFTypeRef;

    pub fn CFRetain(object: CFTypeRef) -> CFTypeRef;
    pub fn CFRelease(object: CFTypeRef);
    pub fn CFArrayCreate(
        allocator: *const c_void,
        values: *const CFTypeRef,
        count: CFIndex,
        callbacks: *const CFArrayCallBacks,
    ) -> CFArrayRef;
    pub fn CFArrayGetCount(array: CFArrayRef) -> CFIndex;
    pub fn CFArrayGetValueAtIndex(array: CFArrayRef, index: CFIndex) -> CFTypeRef;
    pub fn CFDictionaryCreate(
        allocator: *const c_void,
        keys: *const CFTypeRef,
        values: *const CFTypeRef,
        count: CFIndex,
        key_callbacks: *const CFDictionaryCallBacks,
        value_callbacks: *const CFDictionaryCallBacks,
    ) -> CFDictionaryRef;
    pub fn CFDictionarySetValue(dictionary: CFMutableDictionaryRef, key: CFTypeRef, value: CFTypeRef);
    pub fn CFNumberCreate(allocator: *const c_void, number_type: CFIndex, value: *const c_void) -> CFNumberRef;
    pub fn CFDataCreate(allocator: *const c_void, bytes: *const u8, length: CFIndex) -> CFDataRef;
    fn CFStringCreateWithCString(allocator: *const c_void, string: *const c_char, encoding: u32) -> CFStringRef;
}

#[link(name = "CoreGraphics", kind = "framework")]
unsafe extern "C" {
    pub static kCGColorSpaceSRGB: CFStringRef;
    pub static kCGColorSpaceDisplayP3: CFStringRef;
    pub static kCGColorSpaceDisplayP3_PQ: CFStringRef;
    pub static kCGColorSpaceDisplayP3_HLG: CFStringRef;
    pub static kCGColorSpaceITUR_2100_PQ: CFStringRef;
    pub static kCGColorSpaceITUR_2100_HLG: CFStringRef;
    pub static kCGColorSpaceITUR_2020_sRGBGamma: CFStringRef;

    pub fn CGPathCreateMutable() -> CGMutablePathRef;
    pub fn CGPathMoveToPoint(path: CGMutablePathRef, transform: *const CGAffineTransform, x: CGFloat, y: CGFloat);
    pub fn CGPathAddLineToPoint(path: CGMutablePathRef, transform: *const CGAffineTransform, x: CGFloat, y: CGFloat);
    pub fn CGPathAddQuadCurveToPoint(
        path: CGMutablePathRef,
        transform: *const CGAffineTransform,
        control_x: CGFloat,
        control_y: CGFloat,
        x: CGFloat,
        y: CGFloat,
    );
    #[allow(clippy::too_many_arguments)]
    pub fn CGPathAddCurveToPoint(
        path: CGMutablePathRef,
        transform: *const CGAffineTransform,
        first_control_x: CGFloat,
        first_control_y: CGFloat,
        second_control_x: CGFloat,
        second_control_y: CGFloat,
        x: CGFloat,
        y: CGFloat,
    );
    pub fn CGPathCloseSubpath(path: CGMutablePathRef);
    pub fn CGColorCreateSRGB(red: CGFloat, green: CGFloat, blue: CGFloat, alpha: CGFloat) -> CGColorRef;
}

#[link(name = "IOSurface", kind = "framework")]
unsafe extern "C" {
    pub fn IOSurfaceGetWidth(surface: IOSurfaceRef) -> usize;
    pub fn IOSurfaceGetHeight(surface: IOSurfaceRef) -> usize;
    pub fn IOSurfaceIsInUse(surface: IOSurfaceRef) -> u8;
    pub fn IOSurfaceSetValue(surface: IOSurfaceRef, key: CFStringRef, value: CFTypeRef);
}

#[link(name = "CoreVideo", kind = "framework")]
unsafe extern "C" {
    pub static kCVImageBufferColorPrimariesKey: CFStringRef;
    pub static kCVImageBufferTransferFunctionKey: CFStringRef;
    pub static kCVImageBufferYCbCrMatrixKey: CFStringRef;
    pub static kCVImageBufferAmbientViewingEnvironmentKey: CFStringRef;
    pub static kCVImageBufferMasteringDisplayColorVolumeKey: CFStringRef;
    pub static kCVImageBufferContentLightLevelInfoKey: CFStringRef;
    pub static kCVImageBufferCleanApertureKey: CFStringRef;
    pub static kCVImageBufferCleanApertureWidthKey: CFStringRef;
    pub static kCVImageBufferCleanApertureHeightKey: CFStringRef;
    pub static kCVImageBufferCleanApertureHorizontalOffsetKey: CFStringRef;
    pub static kCVImageBufferCleanApertureVerticalOffsetKey: CFStringRef;
    pub static kCVImageBufferYCbCrMatrix_ITU_R_709_2: CFStringRef;
    pub static kCVImageBufferColorPrimaries_ITU_R_709_2: CFStringRef;
    pub static kCVImageBufferTransferFunction_sRGB: CFStringRef;
    pub static kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ: CFStringRef;
    pub static kCVImageBufferTransferFunction_ITU_R_2100_HLG: CFStringRef;

    pub fn CVYCbCrMatrixGetStringForIntegerCodePoint(code_point: i32) -> CFStringRef;
    pub fn CVColorPrimariesGetStringForIntegerCodePoint(code_point: i32) -> CFStringRef;
    pub fn CVTransferFunctionGetStringForIntegerCodePoint(code_point: i32) -> CFStringRef;
    pub fn CVPixelBufferCreateWithIOSurface(
        allocator: *const c_void,
        surface: IOSurfaceRef,
        attributes: CFDictionaryRef,
        pixel_buffer: *mut CVPixelBufferRef,
    ) -> i32;
    pub fn CVBufferSetAttachment(buffer: CVPixelBufferRef, key: CFStringRef, value: CFTypeRef, mode: u32);
}

#[link(name = "CoreMedia", kind = "framework")]
unsafe extern "C" {
    pub static kCMTimeInvalid: CMTime;
    pub static kCMTimeZero: CMTime;
    pub static kCMSampleAttachmentKey_DisplayImmediately: CFStringRef;

    pub fn CMVideoFormatDescriptionCreateForImageBuffer(
        allocator: *const c_void,
        image_buffer: CVPixelBufferRef,
        format_description: *mut CMVideoFormatDescriptionRef,
    ) -> OSStatus;
    pub fn CMSampleBufferCreateReadyWithImageBuffer(
        allocator: *const c_void,
        image_buffer: CVPixelBufferRef,
        format_description: CMVideoFormatDescriptionRef,
        timing: *const CMSampleTimingInfo,
        sample_buffer: *mut CMSampleBufferRef,
    ) -> OSStatus;
    pub fn CMSampleBufferGetSampleAttachmentsArray(
        sample_buffer: CMSampleBufferRef,
        create_if_necessary: u8,
    ) -> CFArrayRef;
}

/// A CoreFoundation object of which this holds one reference, which toll-free bridges to its Objective-C counterpart.
#[derive(Debug, PartialEq, Eq)]
pub struct CfRetained(NonNull<c_void>);

impl CfRetained {
    /// Takes over a reference the caller owns, as one that a `Create` or `Copy` function answers.
    ///
    /// # Safety
    ///
    /// A non-null object must be live, with a reference that the caller hands over.
    pub unsafe fn adopt(object: CFTypeRef) -> Option<CfRetained> {
        NonNull::new(object.cast_mut()).map(CfRetained)
    }

    /// Takes a reference of its own to the object.
    ///
    /// # Safety
    ///
    /// A non-null object must be live.
    pub unsafe fn retain(object: CFTypeRef) -> Option<CfRetained> {
        let object = NonNull::new(object.cast_mut())?;
        // SAFETY: The caller passes a live object.
        unsafe { CFRetain(object.as_ptr()) };
        Some(CfRetained(object))
    }

    pub fn as_ptr(&self) -> CFTypeRef {
        self.0.as_ptr()
    }

    pub fn as_mut_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub fn string(value: &CStr) -> CfRetained {
        // SAFETY: The string is a C string in UTF-8, and the answer is a new string the caller owns.
        unsafe {
            CfRetained::adopt(CFStringCreateWithCString(
                std::ptr::null(),
                value.as_ptr(),
                CF_STRING_ENCODING_UTF8,
            ))
        }
        .expect("a new string")
    }

    pub fn number(value: f64) -> CfRetained {
        // SAFETY: The value is a double, and the answer is a new number the caller owns.
        unsafe {
            CfRetained::adopt(CFNumberCreate(
                std::ptr::null(),
                CF_NUMBER_DOUBLE_TYPE,
                (&raw const value).cast(),
            ))
        }
        .expect("a new number")
    }

    pub fn data(bytes: &[u8]) -> CfRetained {
        // SAFETY: The bytes are copied, and the answer is new data the caller owns.
        unsafe {
            CfRetained::adopt(CFDataCreate(
                std::ptr::null(),
                bytes.as_ptr(),
                CFIndex::try_from(bytes.len()).expect("data fits in a CFIndex"),
            ))
        }
        .expect("new data")
    }

    /// An array of the objects, each of which it retains.
    pub fn array(objects: &[CFTypeRef]) -> CfRetained {
        // SAFETY: The objects are live, the callbacks retain them, and the answer is a new array the caller owns.
        unsafe {
            CfRetained::adopt(CFArrayCreate(
                std::ptr::null(),
                objects.as_ptr(),
                CFIndex::try_from(objects.len()).expect("an array fits in a CFIndex"),
                &raw const kCFTypeArrayCallBacks,
            ))
        }
        .expect("a new array")
    }

    /// A dictionary of the keys and values, each of which it retains.
    pub fn dictionary(entries: &[(CFTypeRef, CFTypeRef)]) -> CfRetained {
        let keys: Vec<CFTypeRef> = entries.iter().map(|(key, _)| *key).collect();
        let values: Vec<CFTypeRef> = entries.iter().map(|(_, value)| *value).collect();
        // SAFETY: The keys and values are live, the callbacks retain them, and the answer is a new dictionary the
        // caller owns.
        unsafe {
            CfRetained::adopt(CFDictionaryCreate(
                std::ptr::null(),
                keys.as_ptr(),
                values.as_ptr(),
                CFIndex::try_from(entries.len()).expect("a dictionary fits in a CFIndex"),
                &raw const kCFTypeDictionaryKeyCallBacks,
                &raw const kCFTypeDictionaryValueCallBacks,
            ))
        }
        .expect("a new dictionary")
    }
}

impl Clone for CfRetained {
    fn clone(&self) -> Self {
        // SAFETY: This holds a reference to a live object.
        unsafe { CfRetained::retain(self.as_ptr()) }.expect("an object")
    }
}

impl Drop for CfRetained {
    fn drop(&mut self) {
        // SAFETY: This gives up the reference it holds.
        unsafe { CFRelease(self.as_ptr()) };
    }
}
