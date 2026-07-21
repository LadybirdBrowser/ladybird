/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust ownership of the ComputedValues style group payloads.
//!
//! Each style value group payload is a C++ struct that Rust treats as an opaque
//! blob: C++ registers a vtable per group with the payload size, alignment and
//! callbacks for default-construction, copy-construction and destruction, in
//! the same way Stylo drives Gecko's nsStyle* structs. Rust owns allocation,
//! layout and destruction; reference counting happens through an atomic header
//! placed immediately before the payload, which the C++ side reads and updates
//! inline so that sharing a payload never crosses the FFI boundary.
//!
//! Layout contract with the C++ side (StyleStructRef):
//!
//!   [ ArcHeader | payload ]
//!               ^-- pointers exchanged over FFI point at the payload
//!
//! The header occupies max(size_of::<usize>(), payload alignment) bytes so the
//! payload stays properly aligned; the reference count lives in the first
//! usize of the header. A reference count of STYLE_GROUP_STATIC_REFCOUNT marks
//! an intentionally leaked payload (the per-group defaults) that must never be
//! reference counted or freed.

use std::alloc::{Layout, alloc, dealloc};
use std::ffi::c_void;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicUsize, Ordering};

use crate::abort_on_panic;

/// Reference count value marking an intentionally leaked payload.
pub const STYLE_GROUP_STATIC_REFCOUNT: usize = usize::MAX;

/// Layout of the inherited box style value group.
///
/// This is the source of truth for the group's payload layout: C++ derives its
/// group struct from the cbindgen mirror of this type, adding the initial
/// values and typed accessors on top. The fields hold C++ `enum class : u8`
/// values that Rust stores as opaque bytes, keeping the enum definitions
/// single-sourced in C++.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct InheritedBoxValues {
    pub visibility: u8,
    pub direction: u8,
    pub writing_mode: u8,
    pub content_visibility: u8,
    pub image_rendering: u8,
}

/// Size, alignment and lifecycle callbacks for one style value group type.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StyleGroupVTable {
    pub size: usize,
    pub align: usize,
    pub default_construct: unsafe extern "C" fn(payload: *mut c_void),
    pub copy_construct: unsafe extern "C" fn(payload: *mut c_void, source: *const c_void),
    pub destruct: unsafe extern "C" fn(payload: *mut c_void),
    /// Field-wise payload equality; groups without a comparable layout
    /// report false, which conservatively disables payload sharing.
    pub equals: unsafe extern "C" fn(a: *const c_void, b: *const c_void) -> bool,
}

// SAFETY: The function pointers are stateless C++ callbacks and the plain
// integers are immutable after registration.
unsafe impl Send for StyleGroupVTable {}
unsafe impl Sync for StyleGroupVTable {}

struct Registry {
    vtables: Box<[StyleGroupVTable]>,
    /// The intentionally leaked per-group default payloads, for building
    /// groups without allocating when every field holds its initial value.
    defaults: Box<[*const c_void]>,
}

// SAFETY: The defaults are immortal, immutable payloads.
unsafe impl Send for Registry {}
unsafe impl Sync for Registry {}

static REGISTRY: OnceLock<Registry> = OnceLock::new();

fn vtable(group_index: usize) -> &'static StyleGroupVTable {
    let registry = REGISTRY.get().expect("style groups used before registration");
    &registry.vtables[group_index]
}

pub(crate) fn default_group_payload(group_index: usize) -> *const c_void {
    REGISTRY.get().expect("style groups used before registration").defaults[group_index]
}

/// Retains one reference to a payload, mirroring StyleStructRef::ref():
/// intentionally leaked payloads are never counted.
pub(crate) fn retain_group_payload(group_index: usize, payload: *const c_void) {
    let refcount = refcount_of(payload, vtable(group_index).align);
    if refcount.load(Ordering::Relaxed) == STYLE_GROUP_STATIC_REFCOUNT {
        return;
    }
    refcount.fetch_add(1, Ordering::Relaxed);
}

fn header_size(align: usize) -> usize {
    align.max(size_of::<usize>())
}

fn allocation_layout(vtable: &StyleGroupVTable) -> Layout {
    let align = vtable.align.max(align_of::<usize>());
    Layout::from_size_align(header_size(vtable.align) + vtable.size, align).expect("style group layout overflow")
}

fn refcount_of(payload: *const c_void, align: usize) -> &'static AtomicUsize {
    // SAFETY: Every payload pointer handed out by this module is preceded by
    // its header, whose first usize is the reference count.
    unsafe {
        let header = (payload as *const u8).sub(header_size(align)) as *const AtomicUsize;
        &*header
    }
}

fn allocate_payload(vtable: &StyleGroupVTable, initial_refcount: usize) -> *mut c_void {
    // SAFETY: The layout is never zero-sized (the header is at least a usize).
    unsafe {
        let allocation = alloc(allocation_layout(vtable));
        if allocation.is_null() {
            std::process::abort();
        }
        let header = allocation as *mut AtomicUsize;
        (*header).store(initial_refcount, Ordering::Relaxed);
        allocation.add(header_size(vtable.align)) as *mut c_void
    }
}

/// Registers the style group vtables and builds the intentionally leaked
/// default payload for every group, written to `out_default_payloads`.
/// Must be called exactly once, before any other function in this module.
///
/// # Safety
/// `vtables` must point at `count` valid vtables and `out_default_payloads`
/// at space for `count` pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_registry_register(
    vtables: *const StyleGroupVTable,
    count: usize,
    out_default_payloads: *mut *const c_void,
) {
    abort_on_panic(|| unsafe {
        let tables: Box<[StyleGroupVTable]> = std::slice::from_raw_parts(vtables, count).into();
        let mut defaults = Vec::with_capacity(count);
        for (index, table) in tables.iter().enumerate() {
            assert!(table.align.is_power_of_two());
            let payload = allocate_payload(table, STYLE_GROUP_STATIC_REFCOUNT);
            (table.default_construct)(payload);
            *out_default_payloads.add(index) = payload;
            defaults.push(payload as *const c_void);
        }
        assert!(
            REGISTRY
                .set(Registry {
                    vtables: tables,
                    defaults: defaults.into_boxed_slice(),
                })
                .is_ok(),
            "style group registry registered twice"
        );
    });
}

/// Allocates a new payload for `group_index` with a reference count of one,
/// copy-constructed from `source`.
///
/// # Safety
/// `source` must be a valid payload of the same group type.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_clone(group_index: usize, source: *const c_void) -> *mut c_void {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::StyleGroupCloneEntry);
    abort_on_panic(|| unsafe {
        let table = vtable(group_index);
        let payload = allocate_payload(table, 1);
        (table.copy_construct)(payload, source);
        payload
    })
}

/// Destroys and deallocates a payload whose reference count has reached zero.
///
/// # Safety
/// `payload` must be a payload of the given group type with no remaining
/// references, and must not be a static default payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_free(group_index: usize, payload: *mut c_void) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::StyleGroupFreeEntry);
    abort_on_panic(|| unsafe {
        let table = vtable(group_index);
        debug_assert!(refcount_of(payload, table.align).load(Ordering::Relaxed) == 0);
        (table.destruct)(payload);
        let allocation = (payload as *mut u8).sub(header_size(table.align));
        dealloc(allocation, allocation_layout(table));
    });
}

/// One field of a style group the generic builder can populate or check: a
/// pokeable simple field (an enum code mapped through a keyword table, a
/// number, or a pixel length) or a constraint requiring a hard field's value
/// to be a specific keyword so the constructor's initial value stands.
#[repr(C)]
pub struct FfiGroupFieldDescriptor {
    pub group_index: u32,
    pub property_id: u16,
    pub offset: u32,
    pub kind: u8,
    /// For GROUP_FIELD_REQUIRE_KEYWORD: the required keyword.
    pub keyword: u16,
    /// For GROUP_FIELD_REQUIRE_PX: the required pixel value.
    pub required_px: f64,
    /// For GROUP_FIELD_ENUM_KEYWORD: keyword code -> enum code, 255 invalid.
    pub keyword_table: *const u8,
    pub keyword_table_length: usize,
}

/// An enum stored as u8, mapped through the descriptor's keyword table.
pub const GROUP_FIELD_ENUM_KEYWORD: u8 = 0;
/// A number stored as f32.
pub const GROUP_FIELD_F32: u8 = 1;
/// A number stored as f64.
pub const GROUP_FIELD_F64: u8 = 2;
/// A pixel length stored as raw CSSPixels (i32).
pub const GROUP_FIELD_CSS_PIXELS: u8 = 3;
/// An integer stored as u64.
pub const GROUP_FIELD_U64: u8 = 4;
/// A constraint: the value must be this keyword; nothing is written.
pub const GROUP_FIELD_REQUIRE_KEYWORD: u8 = 5;
/// An integer stored as i32.
pub const GROUP_FIELD_I32: u8 = 6;
/// A color stored as the C++ Color's raw 32-bit value, resolved by the C++
/// gather loop, which owns the color resolution context.
pub const GROUP_FIELD_COLOR: u8 = 7;
/// A number stored as f32, resolved by the C++ gather loop for values whose
/// normalization has not moved into the core, like opacity.
pub const GROUP_FIELD_RESOLVED_F32: u8 = 8;
/// A constraint: the value must be a pixel length equal to `required_px`;
/// nothing is written.
pub const GROUP_FIELD_REQUIRE_PX: u8 = 9;
/// A color like GROUP_FIELD_COLOR, except that the descriptor's keyword
/// leaves the constructor's initial value standing, for fields like
/// outline-color whose auto keyword is not a resolvable color.
pub const GROUP_FIELD_COLOR_OR_KEYWORD: u8 = 10;
/// A constraint: the value must be the property's initial value, compared by
/// data pointer identity, which holds exactly for untouched properties since
/// the driver selects the initial table's entries directly.
pub const GROUP_FIELD_REQUIRE_INITIAL_VALUE: u8 = 11;
/// A pixel length stored as raw CSSPixels, clamped at zero.
pub const GROUP_FIELD_CSS_PIXELS_NON_NEGATIVE: u8 = 12;
/// A number stored as f64, resolved by the C++ gather loop.
pub const GROUP_FIELD_RESOLVED_F64: u8 = 13;

/// One gathered value for the generic group builder: the computed value's
/// shell and data, plus the resolved raw color for color-kind fields.
#[repr(C)]
pub struct FfiGroupValueEntry {
    pub shell: *const c_void,
    pub data: *const c_void,
    pub resolved_color: u32,
    pub has_resolved_color: bool,
    pub resolved_number: f64,
    pub has_resolved_number: bool,
}

struct FieldDescriptors(Box<[FfiGroupFieldDescriptor]>);

// SAFETY: The keyword tables are immortal C++ statics.
unsafe impl Send for FieldDescriptors {}
unsafe impl Sync for FieldDescriptors {}

static FIELD_DESCRIPTORS: OnceLock<FieldDescriptors> = OnceLock::new();

/// Installs the pokeable-field descriptors for every group in one flat array.
///
/// # Safety
/// `descriptors` must point at `count` valid descriptors whose keyword tables
/// stay alive for the process lifetime.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_register_field_descriptors(
    descriptors: *const FfiGroupFieldDescriptor,
    count: usize,
) {
    abort_on_panic(|| {
        let slice = unsafe { std::slice::from_raw_parts(descriptors, count) };
        let copied: Box<[FfiGroupFieldDescriptor]> = slice
            .iter()
            .map(|descriptor| FfiGroupFieldDescriptor { ..*descriptor })
            .collect();
        assert!(
            FIELD_DESCRIPTORS.set(FieldDescriptors(copied)).is_ok(),
            "field descriptors installed twice"
        );
    });
}

/// Builds a style group payload generically from its registered field
/// descriptors: decodes every descriptor's value (returning null for the C++
/// population path when any value cannot be decoded or a constraint fails),
/// default-constructs a scratch payload, pokes the simple fields, and shares
/// the parent or default payload when the result compares equal.
///
/// # Safety
/// `values` must hold one valid (shell, data) entry per registered descriptor
/// of the group, in registration order; `parent_payload` must be a valid
/// payload of the group or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_style_group(
    group_index: usize,
    values: *const FfiGroupValueEntry,
    count: usize,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::style_value::StyleValueData;

    abort_on_panic(|| {
        let all = &FIELD_DESCRIPTORS.get()?.0;
        let descriptors: Vec<&FfiGroupFieldDescriptor> = all
            .iter()
            .filter(|descriptor| descriptor.group_index as usize == group_index)
            .collect();
        if descriptors.len() != count {
            return None;
        }
        let values = unsafe { std::slice::from_raw_parts(values, count) };

        enum Poke {
            U8(u32, u8),
            F32(u32, f32),
            F64(u32, f64),
            I32(u32, i32),
            U64(u32, u64),
            U32(u32, u32),
        }
        let mut pokes = Vec::with_capacity(count);
        for (descriptor, value) in descriptors.iter().zip(values) {
            let data = unsafe { (value.data as *const StyleValueData).as_ref() }?;
            match descriptor.kind {
                GROUP_FIELD_ENUM_KEYWORD => {
                    let StyleValueData::Keyword { keyword } = data else {
                        return None;
                    };
                    let table = unsafe {
                        std::slice::from_raw_parts(descriptor.keyword_table, descriptor.keyword_table_length)
                    };
                    let code = *table.get(*keyword as usize)?;
                    if code == 255 {
                        return None;
                    }
                    pokes.push(Poke::U8(descriptor.offset, code));
                }
                GROUP_FIELD_F32 => {
                    let StyleValueData::Number { value } = data else {
                        return None;
                    };
                    pokes.push(Poke::F32(descriptor.offset, *value as f32));
                }
                GROUP_FIELD_F64 => {
                    let StyleValueData::Number { value } = data else {
                        return None;
                    };
                    pokes.push(Poke::F64(descriptor.offset, *value));
                }
                GROUP_FIELD_CSS_PIXELS => {
                    let StyleValueData::Length { value, unit } = data else {
                        return None;
                    };
                    if *unit != crate::style_compute::px_length_unit() {
                        return None;
                    }
                    pokes.push(Poke::I32(
                        descriptor.offset,
                        crate::css_pixels::CssPixels::nearest_value_for(*value).raw_value(),
                    ));
                }
                GROUP_FIELD_U64 => {
                    let StyleValueData::Integer { value } = data else {
                        return None;
                    };
                    if *value < 0 {
                        return None;
                    }
                    pokes.push(Poke::U64(descriptor.offset, *value as u64));
                }
                GROUP_FIELD_REQUIRE_KEYWORD => {
                    let StyleValueData::Keyword { keyword } = data else {
                        return None;
                    };
                    if *keyword != descriptor.keyword {
                        return None;
                    }
                }
                GROUP_FIELD_I32 => {
                    let StyleValueData::Integer { value } = data else {
                        return None;
                    };
                    pokes.push(Poke::I32(descriptor.offset, *value));
                }
                GROUP_FIELD_COLOR => {
                    if !value.has_resolved_color {
                        return None;
                    }
                    pokes.push(Poke::U32(descriptor.offset, value.resolved_color));
                }
                GROUP_FIELD_RESOLVED_F32 => {
                    if !value.has_resolved_number {
                        return None;
                    }
                    pokes.push(Poke::F32(descriptor.offset, value.resolved_number as f32));
                }
                GROUP_FIELD_REQUIRE_PX => {
                    let StyleValueData::Length { value, unit } = data else {
                        return None;
                    };
                    if *unit != crate::style_compute::px_length_unit() || *value != descriptor.required_px {
                        return None;
                    }
                }
                GROUP_FIELD_COLOR_OR_KEYWORD => match data {
                    StyleValueData::Keyword { keyword } if *keyword == descriptor.keyword => {}
                    _ => {
                        if !value.has_resolved_color {
                            return None;
                        }
                        pokes.push(Poke::U32(descriptor.offset, value.resolved_color));
                    }
                },
                GROUP_FIELD_REQUIRE_INITIAL_VALUE => {
                    if value.data != crate::style_compute::initial_value(descriptor.property_id).data {
                        return None;
                    }
                }
                GROUP_FIELD_CSS_PIXELS_NON_NEGATIVE => {
                    let StyleValueData::Length { value, unit } = data else {
                        return None;
                    };
                    if *unit != crate::style_compute::px_length_unit() {
                        return None;
                    }
                    pokes.push(Poke::I32(
                        descriptor.offset,
                        crate::css_pixels::CssPixels::nearest_value_for(value.max(0.0)).raw_value(),
                    ));
                }
                GROUP_FIELD_RESOLVED_F64 => {
                    if !value.has_resolved_number {
                        return None;
                    }
                    pokes.push(Poke::F64(descriptor.offset, value.resolved_number));
                }
                _ => return None,
            }
        }

        let table = vtable(group_index);
        let scratch = allocate_payload(table, 1);
        // SAFETY: The scratch payload was allocated for this group's layout,
        // and every poke offset comes from offsetof on the C++ side.
        unsafe {
            (table.default_construct)(scratch);
            for poke in &pokes {
                let base = scratch as *mut u8;
                match *poke {
                    Poke::U8(offset, value) => *base.add(offset as usize) = value,
                    Poke::F32(offset, value) => *(base.add(offset as usize) as *mut f32) = value,
                    Poke::F64(offset, value) => *(base.add(offset as usize) as *mut f64) = value,
                    Poke::I32(offset, value) => *(base.add(offset as usize) as *mut i32) = value,
                    Poke::U64(offset, value) => *(base.add(offset as usize) as *mut u64) = value,
                    Poke::U32(offset, value) => *(base.add(offset as usize) as *mut u32) = value,
                }
            }
        }

        let free_scratch = || unsafe {
            (table.destruct)(scratch);
            let allocation = (scratch as *mut u8).sub(header_size(table.align));
            dealloc(allocation, allocation_layout(table));
        };

        if !parent_payload.is_null() && unsafe { (table.equals)(scratch, parent_payload) } {
            free_scratch();
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if unsafe { (table.equals)(scratch, default_payload) } {
            free_scratch();
            return Some(default_payload);
        }
        Some(scratch as *const c_void)
    })
    .unwrap_or(std::ptr::null())
}

/// Builds an inherited box group payload from the five computed keyword
/// values, sharing instead of allocating whenever it can: the parent's
/// payload when every field matches it, or the immortal default payload when
/// every field holds its initial value. Returns null when any value is not a
/// mappable keyword, in which case the C++ population path applies.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, shared payloads are retained, and default payloads are
/// intentionally leaked and never counted.
///
/// # Safety
/// The value pointers must be valid StyleValueData or null, and
/// `parent_payload` a valid inherited box payload or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_inherited_box_group(
    group_index: usize,
    visibility: *const c_void,
    direction: *const c_void,
    writing_mode: *const c_void,
    content_visibility: *const c_void,
    image_rendering: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::style_value::StyleValueData;

    abort_on_panic(|| {
        let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match unsafe { (data as *const StyleValueData).as_ref() } {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let built = InheritedBoxValues {
            visibility: keyword_code(visibility, crate::style_compute::keyword_to_visibility)?,
            direction: keyword_code(direction, crate::style_compute::keyword_to_direction)?,
            writing_mode: keyword_code(writing_mode, crate::style_compute::keyword_to_writing_mode)?,
            content_visibility: keyword_code(content_visibility, crate::style_compute::keyword_to_content_visibility)?,
            image_rendering: keyword_code(image_rendering, crate::style_compute::keyword_to_image_rendering)?,
        };

        if !parent_payload.is_null() {
            // SAFETY: The caller guarantees a valid inherited box payload.
            if built == unsafe { *(parent_payload as *const InheritedBoxValues) } {
                retain_group_payload(group_index, parent_payload);
                return Some(parent_payload);
            }
        }

        let default_payload = default_group_payload(group_index);
        // SAFETY: The default payload is a valid inherited box payload.
        if built == unsafe { *(default_payload as *const InheritedBoxValues) } {
            return Some(default_payload);
        }

        let payload = allocate_payload(vtable(group_index), 1);
        // SAFETY: The payload was allocated for this group's layout.
        unsafe { *(payload as *mut InheritedBoxValues) = built };
        Some(payload as *const c_void)
    })
    .unwrap_or(std::ptr::null())
}

/// Builds an inherited table group payload from the computed values, with the
/// same sharing rules as the inherited box builder. Border-spacing must be an
/// absolute pixel length; two-value spacings and anything else fall back to
/// the C++ population path by returning null.
///
/// # Safety
/// The value pointers must be valid StyleValueData or null, and
/// `parent_payload` a valid inherited table payload or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_inherited_table_group(
    group_index: usize,
    border_collapse: *const c_void,
    caption_side: *const c_void,
    empty_cells: *const c_void,
    border_spacing: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::style_value::StyleValueData;

    abort_on_panic(|| {
        let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match unsafe { (data as *const StyleValueData).as_ref() } {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let spacing = match unsafe { (border_spacing as *const StyleValueData).as_ref() } {
            Some(StyleValueData::Length { value, unit }) if *unit == crate::style_compute::px_length_unit() => {
                crate::css_pixels::CssPixels::nearest_value_for(*value).raw_value()
            }
            _ => return None,
        };
        let built = InheritedTableValues {
            border_collapse: keyword_code(border_collapse, crate::style_compute::keyword_to_border_collapse)?,
            caption_side: keyword_code(caption_side, crate::style_compute::keyword_to_caption_side)?,
            empty_cells: keyword_code(empty_cells, crate::style_compute::keyword_to_empty_cells)?,
            border_spacing_horizontal: spacing,
            border_spacing_vertical: spacing,
        };

        if !parent_payload.is_null() {
            // SAFETY: The caller guarantees a valid inherited table payload.
            if built == unsafe { *(parent_payload as *const InheritedTableValues) } {
                retain_group_payload(group_index, parent_payload);
                return Some(parent_payload);
            }
        }

        let default_payload = default_group_payload(group_index);
        // SAFETY: The default payload is a valid inherited table payload.
        if built == unsafe { *(default_payload as *const InheritedTableValues) } {
            return Some(default_payload);
        }

        let payload = allocate_payload(vtable(group_index), 1);
        // SAFETY: The payload was allocated for this group's layout.
        unsafe { *(payload as *mut InheritedTableValues) = built };
        Some(payload as *const c_void)
    })
    .unwrap_or(std::ptr::null())
}

/// Layout of the inherited table style value group.
///
/// The enum fields follow the opaque-byte convention; the border spacings are
/// raw CSSPixels fixed-point values.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct InheritedTableValues {
    pub border_collapse: u8,
    pub caption_side: u8,
    pub empty_cells: u8,
    pub border_spacing_horizontal: i32,
    pub border_spacing_vertical: i32,
}

/// Returns the typed view of an inherited table group payload.
///
/// # Safety
/// `payload` must be an inherited table group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_inherited_table(payload: *const c_void) -> *const InheritedTableValues {
    payload as *const InheritedTableValues
}

/// Returns the typed view of an inherited box group payload.
///
/// This anchors the Rust-defined layout in the exported ABI so the cbindgen
/// mirror stays in the generated header; Rust-side style computation reads and
/// writes group payloads through these typed views.
///
/// # Safety
/// `payload` must be an inherited box group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_inherited_box(payload: *const c_void) -> *const InheritedBoxValues {
    payload as *const InheritedBoxValues
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize as TestCounter;

    static LIVE: TestCounter = TestCounter::new(0);

    unsafe extern "C" fn test_default_construct(payload: *mut c_void) {
        unsafe { *(payload as *mut u64) = 7 };
        LIVE.fetch_add(1, Ordering::Relaxed);
    }
    unsafe extern "C" fn test_copy_construct(payload: *mut c_void, source: *const c_void) {
        unsafe { *(payload as *mut u64) = *(source as *const u64) };
        LIVE.fetch_add(1, Ordering::Relaxed);
    }
    unsafe extern "C" fn test_destruct(_payload: *mut c_void) {
        LIVE.fetch_sub(1, Ordering::Relaxed);
    }
    unsafe extern "C" fn test_equals(a: *const c_void, b: *const c_void) -> bool {
        unsafe { *(a as *const u64) == *(b as *const u64) }
    }

    #[test]
    fn payload_lifecycle() {
        let vtables = [StyleGroupVTable {
            size: size_of::<u64>(),
            align: align_of::<u64>(),
            default_construct: test_default_construct,
            copy_construct: test_copy_construct,
            destruct: test_destruct,
            equals: test_equals,
        }];
        let mut defaults = [std::ptr::null::<c_void>(); 1];
        unsafe {
            rust_style_group_registry_register(vtables.as_ptr(), 1, defaults.as_mut_ptr());
            let default_payload = defaults[0];
            assert_eq!(*(default_payload as *const u64), 7);
            assert_eq!(
                refcount_of(default_payload, align_of::<u64>()).load(Ordering::Relaxed),
                STYLE_GROUP_STATIC_REFCOUNT
            );

            let clone = rust_style_group_clone(0, default_payload);
            assert_eq!(*(clone as *const u64), 7);
            let refcount = refcount_of(clone, align_of::<u64>());
            assert_eq!(refcount.load(Ordering::Relaxed), 1);

            refcount.store(0, Ordering::Relaxed);
            rust_style_group_free(0, clone);
            assert_eq!(LIVE.load(Ordering::Relaxed), 1);
        }
    }
}
