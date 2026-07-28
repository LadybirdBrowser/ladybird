/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust ownership of the ComputedValues style group payloads.
//!
//! Rust-native style groups define their payload layout and lifecycle here.
//! Groups that still contain C++-owned field types register their payload size,
//! alignment and lifecycle callbacks, in the same way Stylo drives Gecko's
//! nsStyle* structs. Rust owns allocation and reference counting for both kinds;
//! the atomic header is placed immediately before the payload, which the C++
//! side reads and updates inline so that sharing never crosses the FFI boundary.
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
pub use crate::css::computed_value_types::{
    AlignmentValues, ComputedFlexBasis, ComputedGap, ComputedLengthBox, ComputedLengthPercentageOrAuto, ComputedSize,
    ComputedSizeKind, ComputedStyleValueHandle, SVGResetValues, SizingValues, SurroundValues,
};

/// Reference count value marking an intentionally leaked payload.
pub const STYLE_GROUP_STATIC_REFCOUNT: usize = usize::MAX;

/// Layout of the inherited box style value group.
///
/// This is the source of truth for the group's payload layout: C++ derives its
/// group struct from the cbindgen mirror of this type, adding its C++ identity
/// and typed accessors on top. The fields hold C++ `enum class : u8` values
/// generated from CSS/Enums.json, the same source as the C++ enums.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InheritedBoxValues {
    pub visibility: u8,
    pub direction: u8,
    pub writing_mode: u8,
    pub content_visibility: u8,
    pub image_rendering: u8,
}

impl ComputedStyleValueHandle {
    fn empty() -> Self {
        Self {
            pointer: std::ptr::null(),
        }
    }

    fn retained(data: *const crate::css::style_value::StyleValueData) -> Self {
        Self {
            pointer: unsafe { crate::css::style_value::rust_style_value_retain(data) }.cast(),
        }
    }

    fn length(value: f64) -> Self {
        Self {
            pointer: crate::css::style_value::rust_style_value_create_length(
                value,
                crate::css::style_compute::px_length_unit(),
            )
            .cast(),
        }
    }

    fn data(&self) -> Option<&crate::css::style_value::StyleValueData> {
        unsafe { self.pointer.cast::<crate::css::style_value::StyleValueData>().as_ref() }
    }
}

impl Clone for ComputedStyleValueHandle {
    fn clone(&self) -> Self {
        Self {
            pointer: unsafe { crate::css::style_value::rust_style_value_retain(self.pointer.cast()) }.cast(),
        }
    }
}

impl Drop for ComputedStyleValueHandle {
    fn drop(&mut self) {
        unsafe { crate::css::style_value::rust_style_value_release(self.pointer.cast()) };
    }
}

impl PartialEq for ComputedStyleValueHandle {
    fn eq(&self, other: &Self) -> bool {
        match (self.data(), other.data()) {
            (Some(first), Some(second)) => std::ptr::eq(first, second) || first == second,
            (None, None) => true,
            _ => false,
        }
    }
}

impl Clone for ComputedSize {
    fn clone(&self) -> Self {
        Self {
            kind: self.kind,
            value: self.value.clone(),
        }
    }
}

impl PartialEq for ComputedSize {
    fn eq(&self, other: &Self) -> bool {
        self.kind == other.kind && self.value == other.value
    }
}

macro_rules! impl_computed_payload_clone_and_eq {
    ($type:ty { $($field:ident),+ $(,)? }) => {
        impl Clone for $type {
            fn clone(&self) -> Self {
                Self {
                    $($field: self.$field.clone(),)+
                }
            }
        }

        impl PartialEq for $type {
            fn eq(&self, other: &Self) -> bool {
                true $(&& self.$field == other.$field)+
            }
        }
    };
}

impl_computed_payload_clone_and_eq!(SizingValues {
    width,
    min_width,
    max_width,
    height,
    min_height,
    max_height,
});
impl_computed_payload_clone_and_eq!(ComputedFlexBasis { is_content, size });
impl_computed_payload_clone_and_eq!(ComputedGap { is_normal, value });
impl_computed_payload_clone_and_eq!(AlignmentValues {
    flex_direction,
    flex_wrap,
    flex_basis,
    flex_grow,
    flex_shrink,
    order,
    align_content,
    align_items,
    align_self,
    justify_content,
    justify_items,
    justify_self,
    column_gap,
    row_gap,
});
impl_computed_payload_clone_and_eq!(ComputedLengthPercentageOrAuto { is_auto, value });
impl_computed_payload_clone_and_eq!(ComputedLengthBox {
    top,
    right,
    bottom,
    left,
});
impl_computed_payload_clone_and_eq!(SurroundValues {
    inset,
    top_anchor_inset,
    right_anchor_inset,
    bottom_anchor_inset,
    left_anchor_inset,
    margin,
    padding,
});
impl_computed_payload_clone_and_eq!(SVGResetValues {
    cx,
    cy,
    r,
    rx,
    ry,
    x,
    y,
    stop_color,
    stop_opacity,
    flood_color,
    flood_opacity,
    vector_effect,
    shape_rendering,
});

/// Selects the language that owns a style group's payload lifecycle.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum StyleGroupLifecycle {
    Cpp,
    InheritedTable,
    InheritedBox,
    Sizing,
    Alignment,
    SVGReset,
    Surround,
}

/// Size, alignment and optional C++ lifecycle callbacks for one style value
/// group type. Rust-native groups leave the callbacks null.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StyleGroupVTable {
    pub lifecycle: StyleGroupLifecycle,
    pub size: usize,
    pub align: usize,
    pub default_construct: Option<unsafe extern "C" fn(payload: *mut c_void)>,
    pub copy_construct: Option<unsafe extern "C" fn(payload: *mut c_void, source: *const c_void)>,
    pub destruct: Option<unsafe extern "C" fn(payload: *mut c_void)>,
    /// Field-wise payload equality; groups without a comparable layout
    /// report false, which conservatively disables payload sharing.
    pub equals: Option<unsafe extern "C" fn(a: *const c_void, b: *const c_void) -> bool>,
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

/// Resolves the registered group index of a Rust-native style group, letting
/// the layout engine locate its typed payloads without a C++-supplied schema.
pub(crate) fn style_group_index_with_lifecycle(lifecycle: StyleGroupLifecycle) -> usize {
    let registry = REGISTRY.get().expect("style groups used before registration");
    registry
        .vtables
        .iter()
        .position(|table| table.lifecycle == lifecycle)
        .expect("style group lifecycle not registered")
}

pub(crate) fn registered_style_group_count() -> usize {
    REGISTRY
        .get()
        .expect("style groups used before registration")
        .vtables
        .len()
}

fn payload_size(table: &StyleGroupVTable) -> usize {
    match table.lifecycle {
        StyleGroupLifecycle::Cpp => table.size,
        StyleGroupLifecycle::InheritedTable => size_of::<InheritedTableValues>(),
        StyleGroupLifecycle::InheritedBox => size_of::<InheritedBoxValues>(),
        StyleGroupLifecycle::Sizing => size_of::<SizingValues>(),
        StyleGroupLifecycle::Alignment => size_of::<AlignmentValues>(),
        StyleGroupLifecycle::SVGReset => size_of::<SVGResetValues>(),
        StyleGroupLifecycle::Surround => size_of::<SurroundValues>(),
    }
}

fn payload_align(table: &StyleGroupVTable) -> usize {
    match table.lifecycle {
        StyleGroupLifecycle::Cpp => table.align,
        StyleGroupLifecycle::InheritedTable => align_of::<InheritedTableValues>(),
        StyleGroupLifecycle::InheritedBox => align_of::<InheritedBoxValues>(),
        StyleGroupLifecycle::Sizing => align_of::<SizingValues>(),
        StyleGroupLifecycle::Alignment => align_of::<AlignmentValues>(),
        StyleGroupLifecycle::SVGReset => align_of::<SVGResetValues>(),
        StyleGroupLifecycle::Surround => align_of::<SurroundValues>(),
    }
}

unsafe fn default_construct(table: &StyleGroupVTable, payload: *mut c_void) {
    match table.lifecycle {
        StyleGroupLifecycle::Cpp => unsafe {
            table.default_construct.expect("missing C++ style group constructor")(payload);
        },
        StyleGroupLifecycle::InheritedTable => unsafe {
            (payload as *mut InheritedTableValues).write(InheritedTableValues::initial());
        },
        StyleGroupLifecycle::InheritedBox => unsafe {
            (payload as *mut InheritedBoxValues).write(InheritedBoxValues::initial());
        },
        StyleGroupLifecycle::Sizing => unsafe {
            (payload as *mut SizingValues).write(SizingValues::initial());
        },
        StyleGroupLifecycle::Alignment => unsafe {
            (payload as *mut AlignmentValues).write(AlignmentValues::initial());
        },
        StyleGroupLifecycle::SVGReset => unsafe {
            (payload as *mut SVGResetValues).write(SVGResetValues::initial());
        },
        StyleGroupLifecycle::Surround => unsafe {
            (payload as *mut SurroundValues).write(SurroundValues::initial());
        },
    }
}

unsafe fn copy_construct(table: &StyleGroupVTable, payload: *mut c_void, source: *const c_void) {
    match table.lifecycle {
        StyleGroupLifecycle::Cpp => unsafe {
            table.copy_construct.expect("missing C++ style group copy constructor")(payload, source);
        },
        StyleGroupLifecycle::InheritedTable => unsafe {
            (payload as *mut InheritedTableValues).write(*(source as *const InheritedTableValues));
        },
        StyleGroupLifecycle::InheritedBox => unsafe {
            (payload as *mut InheritedBoxValues).write(*(source as *const InheritedBoxValues));
        },
        StyleGroupLifecycle::Sizing => unsafe {
            (payload as *mut SizingValues).write((*(source as *const SizingValues)).clone());
        },
        StyleGroupLifecycle::Alignment => unsafe {
            (payload as *mut AlignmentValues).write((*(source as *const AlignmentValues)).clone());
        },
        StyleGroupLifecycle::SVGReset => unsafe {
            (payload as *mut SVGResetValues).write((*(source as *const SVGResetValues)).clone());
        },
        StyleGroupLifecycle::Surround => unsafe {
            (payload as *mut SurroundValues).write((*(source as *const SurroundValues)).clone());
        },
    }
}

unsafe fn destruct(table: &StyleGroupVTable, payload: *mut c_void) {
    match table.lifecycle {
        StyleGroupLifecycle::Cpp => unsafe { table.destruct.expect("missing C++ style group destructor")(payload) },
        StyleGroupLifecycle::InheritedTable => unsafe { std::ptr::drop_in_place(payload as *mut InheritedTableValues) },
        StyleGroupLifecycle::InheritedBox => unsafe { std::ptr::drop_in_place(payload as *mut InheritedBoxValues) },
        StyleGroupLifecycle::Sizing => unsafe { std::ptr::drop_in_place(payload as *mut SizingValues) },
        StyleGroupLifecycle::Alignment => unsafe { std::ptr::drop_in_place(payload as *mut AlignmentValues) },
        StyleGroupLifecycle::SVGReset => unsafe { std::ptr::drop_in_place(payload as *mut SVGResetValues) },
        StyleGroupLifecycle::Surround => unsafe { std::ptr::drop_in_place(payload as *mut SurroundValues) },
    }
}

unsafe fn payloads_equal(table: &StyleGroupVTable, a: *const c_void, b: *const c_void) -> bool {
    match table.lifecycle {
        StyleGroupLifecycle::Cpp => unsafe { table.equals.is_some_and(|equals| equals(a, b)) },
        StyleGroupLifecycle::InheritedTable => unsafe {
            *(a as *const InheritedTableValues) == *(b as *const InheritedTableValues)
        },
        StyleGroupLifecycle::InheritedBox => unsafe {
            *(a as *const InheritedBoxValues) == *(b as *const InheritedBoxValues)
        },
        StyleGroupLifecycle::Sizing => unsafe { *(a as *const SizingValues) == *(b as *const SizingValues) },
        StyleGroupLifecycle::Alignment => unsafe { *(a as *const AlignmentValues) == *(b as *const AlignmentValues) },
        StyleGroupLifecycle::SVGReset => unsafe { *(a as *const SVGResetValues) == *(b as *const SVGResetValues) },
        StyleGroupLifecycle::Surround => unsafe { *(a as *const SurroundValues) == *(b as *const SurroundValues) },
    }
}

pub(crate) fn default_group_payload(group_index: usize) -> *const c_void {
    REGISTRY.get().expect("style groups used before registration").defaults[group_index]
}

/// Retains one reference to a payload, mirroring StyleStructRef::ref():
/// intentionally leaked payloads are never counted.
pub(crate) fn retain_group_payload(group_index: usize, payload: *const c_void) {
    let refcount = refcount_of(payload, payload_align(vtable(group_index)));
    if refcount.load(Ordering::Relaxed) == STYLE_GROUP_STATIC_REFCOUNT {
        return;
    }
    refcount.fetch_add(1, Ordering::Relaxed);
}

fn header_size(align: usize) -> usize {
    align.max(size_of::<usize>())
}

fn allocation_layout(vtable: &StyleGroupVTable) -> Layout {
    let payload_align = payload_align(vtable);
    let align = payload_align.max(align_of::<usize>());
    Layout::from_size_align(header_size(payload_align) + payload_size(vtable), align)
        .expect("style group layout overflow")
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
        allocation.add(header_size(payload_align(vtable))) as *mut c_void
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
            assert!(payload_align(table).is_power_of_two());
            assert_eq!(table.size, payload_size(table), "style group size disagrees across FFI");
            assert_eq!(
                table.align,
                payload_align(table),
                "style group alignment disagrees across FFI"
            );
            let payload = allocate_payload(table, STYLE_GROUP_STATIC_REFCOUNT);
            default_construct(table, payload);
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleGroupCloneEntry);
    abort_on_panic(|| unsafe {
        let table = vtable(group_index);
        let payload = allocate_payload(table, 1);
        copy_construct(table, payload, source);
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleGroupFreeEntry);
    abort_on_panic(|| unsafe {
        let table = vtable(group_index);
        debug_assert!(refcount_of(payload, payload_align(table)).load(Ordering::Relaxed) == 0);
        destruct(table, payload);
        let allocation = (payload as *mut u8).sub(header_size(payload_align(table)));
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
/// The value's Rust data stored into a single-pointer handle slot, retaining
/// one reference; the slot's constructor default must be null.
pub const GROUP_FIELD_RETAINED_DATA: u8 = 14;
/// A bool stored as one byte: whether the value is the descriptor's keyword.
pub const GROUP_FIELD_KEYWORD_EQUALS_BOOL: u8 = 15;
/// A byte resolved by the C++ gather loop, carried in the resolved number,
/// for derived enum fields like the used color-scheme.
pub const GROUP_FIELD_RESOLVED_U8: u8 = 16;

/// One gathered value for the generic group builder: the computed value's
/// data, plus the resolved raw color for color-kind fields.
#[repr(C)]
pub struct FfiGroupValueEntry {
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
/// `values` must hold one valid data entry per registered descriptor
/// of the group, in registration order; `parent_payload` must be a valid
/// payload of the group or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_style_group(
    group_index: usize,
    values: *const FfiGroupValueEntry,
    count: usize,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

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
            Data(u32, *const StyleValueData),
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
                    if *unit != crate::css::style_compute::px_length_unit() {
                        return None;
                    }
                    pokes.push(Poke::I32(
                        descriptor.offset,
                        crate::css::css_pixels::CssPixels::nearest_value_for(*value).raw_value(),
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
                    // NB: Repeatable-list properties keep even a single computed item in a value list.
                    let data = match data {
                        StyleValueData::ValueList { values, .. } if values.as_slice().len() == 1 => {
                            values.as_slice()[0].data()
                        }
                        data => data,
                    };
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
                    if *unit != crate::css::style_compute::px_length_unit() || *value != descriptor.required_px {
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
                    if value.data != crate::css::style_compute::initial_value_data(descriptor.property_id).cast() {
                        return None;
                    }
                }
                GROUP_FIELD_CSS_PIXELS_NON_NEGATIVE => {
                    let StyleValueData::Length { value, unit } = data else {
                        return None;
                    };
                    if *unit != crate::css::style_compute::px_length_unit() {
                        return None;
                    }
                    pokes.push(Poke::I32(
                        descriptor.offset,
                        crate::css::css_pixels::CssPixels::nearest_value_for(value.max(0.0)).raw_value(),
                    ));
                }
                GROUP_FIELD_RESOLVED_F64 => {
                    if !value.has_resolved_number {
                        return None;
                    }
                    pokes.push(Poke::F64(descriptor.offset, value.resolved_number));
                }
                GROUP_FIELD_RETAINED_DATA => {
                    pokes.push(Poke::Data(descriptor.offset, value.data.cast()));
                }
                GROUP_FIELD_KEYWORD_EQUALS_BOOL => {
                    let is_keyword =
                        matches!(data, StyleValueData::Keyword { keyword } if *keyword == descriptor.keyword);
                    pokes.push(Poke::U8(descriptor.offset, is_keyword as u8));
                }
                GROUP_FIELD_RESOLVED_U8 => {
                    if !value.has_resolved_number {
                        return None;
                    }
                    pokes.push(Poke::U8(descriptor.offset, value.resolved_number as u8));
                }
                _ => return None,
            }
        }

        let table = vtable(group_index);
        let scratch = allocate_payload(table, 1);
        // SAFETY: The scratch payload was allocated for this group's layout,
        // and every poke offset comes from offsetof on the C++ side.
        unsafe {
            default_construct(table, scratch);
            for poke in &pokes {
                let base = scratch as *mut u8;
                match *poke {
                    Poke::U8(offset, value) => *base.add(offset as usize) = value,
                    Poke::F32(offset, value) => *(base.add(offset as usize) as *mut f32) = value,
                    Poke::F64(offset, value) => *(base.add(offset as usize) as *mut f64) = value,
                    Poke::I32(offset, value) => *(base.add(offset as usize) as *mut i32) = value,
                    Poke::U64(offset, value) => *(base.add(offset as usize) as *mut u64) = value,
                    Poke::U32(offset, value) => *(base.add(offset as usize) as *mut u32) = value,
                    Poke::Data(offset, data) => {
                        // The slot's constructor default is null, so nothing is released.
                        let retained = crate::css::style_value::rust_style_value_retain(data);
                        *(base.add(offset as usize) as *mut *const StyleValueData) = retained;
                    }
                }
            }
        }

        let free_scratch = || unsafe {
            destruct(table, scratch);
            let allocation = (scratch as *mut u8).sub(header_size(payload_align(table)));
            dealloc(allocation, allocation_layout(table));
        };

        if !parent_payload.is_null() && unsafe { payloads_equal(table, scratch, parent_payload) } {
            free_scratch();
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if unsafe { payloads_equal(table, scratch, default_payload) } {
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
    use crate::css::style_value::StyleValueData;

    abort_on_panic(|| {
        let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match unsafe { (data as *const StyleValueData).as_ref() } {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let built = InheritedBoxValues {
            visibility: keyword_code(visibility, crate::css::style_compute::keyword_to_visibility)?,
            direction: keyword_code(direction, crate::css::style_compute::keyword_to_direction)?,
            writing_mode: keyword_code(writing_mode, crate::css::style_compute::keyword_to_writing_mode)?,
            content_visibility: keyword_code(
                content_visibility,
                crate::css::style_compute::keyword_to_content_visibility,
            )?,
            image_rendering: keyword_code(image_rendering, crate::css::style_compute::keyword_to_image_rendering)?,
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

impl ComputedSize {
    fn keyword(kind: ComputedSizeKind) -> Self {
        Self {
            kind,
            value: ComputedStyleValueHandle::empty(),
        }
    }

    fn retained(kind: ComputedSizeKind, data: *const crate::css::style_value::StyleValueData) -> Self {
        Self {
            kind,
            value: ComputedStyleValueHandle::retained(data),
        }
    }

    fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        match unsafe { data.as_ref() } {
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::AUTO => {
                Self::keyword(ComputedSizeKind::Auto)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::FIT_CONTENT => {
                Self::keyword(ComputedSizeKind::FitContent)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::MIN_CONTENT => {
                Self::keyword(ComputedSizeKind::MinContent)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::MAX_CONTENT => {
                Self::keyword(ComputedSizeKind::MaxContent)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::NONE => {
                Self::keyword(ComputedSizeKind::None)
            }
            Some(StyleValueData::Function { value, .. }) => {
                Self::retained(ComputedSizeKind::FitContent, value.pointer())
            }
            Some(StyleValueData::Calculated { .. }) => Self::retained(ComputedSizeKind::Calculated, data),
            Some(StyleValueData::Percentage { .. }) => Self::retained(ComputedSizeKind::Percentage, data),
            Some(StyleValueData::Length { .. }) => Self::retained(ComputedSizeKind::Length, data),
            // FIXME: Support `anchor-size(..)`.
            Some(StyleValueData::AnchorSize { .. }) => Self::keyword(ComputedSizeKind::None),
            _ => Self::keyword(ComputedSizeKind::Auto),
        }
    }
}

impl SizingValues {
    fn initial() -> Self {
        Self {
            width: ComputedSize::keyword(ComputedSizeKind::Auto),
            min_width: ComputedSize::keyword(ComputedSizeKind::Auto),
            max_width: ComputedSize::keyword(ComputedSizeKind::None),
            height: ComputedSize::keyword(ComputedSizeKind::Auto),
            min_height: ComputedSize::keyword(ComputedSizeKind::Auto),
            max_height: ComputedSize::keyword(ComputedSizeKind::None),
        }
    }
}

impl ComputedFlexBasis {
    fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let is_content = matches!(
            unsafe { data.cast::<StyleValueData>().as_ref() },
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::CONTENT
        );
        Self {
            is_content,
            size: if is_content {
                ComputedSize::keyword(ComputedSizeKind::Auto)
            } else {
                ComputedSize::from_data(data)
            },
        }
    }
}

impl ComputedGap {
    fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        let is_normal = matches!(
            unsafe { data.as_ref() },
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL
        );
        Self {
            is_normal,
            value: if is_normal {
                ComputedStyleValueHandle::empty()
            } else {
                ComputedStyleValueHandle::retained(data)
            },
        }
    }
}

impl AlignmentValues {
    fn initial() -> Self {
        use crate::css::css_enums::{
            align_content, align_items, align_self, flex_direction, flex_wrap, justify_content, justify_items,
            justify_self,
        };

        Self {
            flex_direction: flex_direction::ROW,
            flex_wrap: flex_wrap::NOWRAP,
            flex_basis: ComputedFlexBasis {
                is_content: false,
                size: ComputedSize::keyword(ComputedSizeKind::Auto),
            },
            flex_grow: 0.0,
            flex_shrink: 1.0,
            order: 0,
            align_content: align_content::STRETCH,
            align_items: align_items::STRETCH,
            align_self: align_self::AUTO,
            justify_content: justify_content::FLEX_START,
            justify_items: justify_items::LEGACY,
            justify_self: justify_self::AUTO,
            column_gap: ComputedGap {
                is_normal: true,
                value: ComputedStyleValueHandle::empty(),
            },
            row_gap: ComputedGap {
                is_normal: true,
                value: ComputedStyleValueHandle::empty(),
            },
        }
    }
}

impl ComputedLengthPercentageOrAuto {
    fn auto() -> Self {
        Self {
            is_auto: true,
            value: ComputedStyleValueHandle::empty(),
        }
    }

    fn zero() -> Self {
        Self {
            is_auto: false,
            value: ComputedStyleValueHandle::length(0.0),
        }
    }

    fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        let is_auto = matches!(
            unsafe { data.as_ref() },
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::AUTO
        );
        Self {
            is_auto,
            value: if is_auto {
                ComputedStyleValueHandle::empty()
            } else {
                ComputedStyleValueHandle::retained(data)
            },
        }
    }

    fn from_length_box_data(data: *const c_void, default_is_auto: bool) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        match unsafe { data.as_ref() } {
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::AUTO => Self::auto(),
            Some(
                StyleValueData::Length { .. } | StyleValueData::Percentage { .. } | StyleValueData::Calculated { .. },
            ) => Self {
                is_auto: false,
                value: ComputedStyleValueHandle::retained(data),
            },
            _ if default_is_auto => Self::auto(),
            _ => Self::zero(),
        }
    }
}

impl ComputedLengthBox {
    fn auto() -> Self {
        Self {
            top: ComputedLengthPercentageOrAuto::auto(),
            right: ComputedLengthPercentageOrAuto::auto(),
            bottom: ComputedLengthPercentageOrAuto::auto(),
            left: ComputedLengthPercentageOrAuto::auto(),
        }
    }

    fn zero() -> Self {
        let zero = ComputedStyleValueHandle::length(0.0);
        let side = || ComputedLengthPercentageOrAuto {
            is_auto: false,
            value: zero.clone(),
        };
        Self {
            top: side(),
            right: side(),
            bottom: side(),
            left: side(),
        }
    }

    fn from_data(
        top: *const c_void,
        right: *const c_void,
        bottom: *const c_void,
        left: *const c_void,
        default_is_auto: bool,
    ) -> Self {
        Self {
            top: ComputedLengthPercentageOrAuto::from_length_box_data(top, default_is_auto),
            right: ComputedLengthPercentageOrAuto::from_length_box_data(right, default_is_auto),
            bottom: ComputedLengthPercentageOrAuto::from_length_box_data(bottom, default_is_auto),
            left: ComputedLengthPercentageOrAuto::from_length_box_data(left, default_is_auto),
        }
    }
}

impl SurroundValues {
    fn initial() -> Self {
        Self {
            inset: ComputedLengthBox::auto(),
            top_anchor_inset: ComputedStyleValueHandle::empty(),
            right_anchor_inset: ComputedStyleValueHandle::empty(),
            bottom_anchor_inset: ComputedStyleValueHandle::empty(),
            left_anchor_inset: ComputedStyleValueHandle::empty(),
            margin: ComputedLengthBox::zero(),
            padding: ComputedLengthBox::zero(),
        }
    }
}

impl SVGResetValues {
    fn initial() -> Self {
        use crate::css::css_enums::{shape_rendering, vector_effect};

        const OPAQUE_BLACK_BGRA: u32 = 0xff00_0000;

        let zero = ComputedStyleValueHandle::length(0.0);
        Self {
            cx: zero.clone(),
            cy: zero.clone(),
            r: zero.clone(),
            rx: ComputedLengthPercentageOrAuto {
                is_auto: true,
                value: ComputedStyleValueHandle::empty(),
            },
            ry: ComputedLengthPercentageOrAuto {
                is_auto: true,
                value: ComputedStyleValueHandle::empty(),
            },
            x: zero.clone(),
            y: zero,
            stop_color: OPAQUE_BLACK_BGRA,
            stop_opacity: 1.0,
            flood_color: OPAQUE_BLACK_BGRA,
            flood_opacity: 1.0,
            vector_effect: vector_effect::NONE,
            shape_rendering: shape_rendering::AUTO,
        }
    }
}

/// Builds the complete alignment group from its computed property values.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, shared payloads are retained, and default payloads are
/// intentionally leaked and never counted.
///
/// # Safety
/// The value pointers must identify valid StyleValueData, and
/// `parent_payload` must identify an alignment payload or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_alignment_group(
    group_index: usize,
    flex_direction: *const c_void,
    flex_wrap: *const c_void,
    flex_basis: *const c_void,
    flex_grow: f64,
    flex_shrink: f64,
    order: i32,
    align_content: *const c_void,
    align_items: *const c_void,
    align_self: *const c_void,
    justify_content: *const c_void,
    justify_items: *const c_void,
    justify_self: *const c_void,
    column_gap: *const c_void,
    row_gap: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    abort_on_panic(|| {
        let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match unsafe { data.cast::<StyleValueData>().as_ref() } {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let built = AlignmentValues {
            flex_direction: keyword_code(flex_direction, crate::css::css_enums::keyword_to_flex_direction)?,
            flex_wrap: keyword_code(flex_wrap, crate::css::css_enums::keyword_to_flex_wrap)?,
            flex_basis: ComputedFlexBasis::from_data(flex_basis),
            flex_grow,
            flex_shrink,
            order,
            align_content: keyword_code(align_content, crate::css::css_enums::keyword_to_align_content)?,
            align_items: keyword_code(align_items, crate::css::css_enums::keyword_to_align_items)?,
            align_self: keyword_code(align_self, crate::css::css_enums::keyword_to_align_self)?,
            justify_content: keyword_code(justify_content, crate::css::css_enums::keyword_to_justify_content)?,
            justify_items: keyword_code(justify_items, crate::css::css_enums::keyword_to_justify_items)?,
            justify_self: keyword_code(justify_self, crate::css::css_enums::keyword_to_justify_self)?,
            column_gap: ComputedGap::from_data(column_gap),
            row_gap: ComputedGap::from_data(row_gap),
        };

        if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<AlignmentValues>() }) {
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if built.eq(unsafe { &*default_payload.cast::<AlignmentValues>() }) {
            return Some(default_payload);
        }

        let payload = allocate_payload(vtable(group_index), 1);
        unsafe { payload.cast::<AlignmentValues>().write(built) };
        Some(payload.cast_const())
    })
    .unwrap_or(std::ptr::null())
}

/// Builds the complete non-inherited SVG geometry and painting group from its
/// computed property values.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, shared payloads are retained, and default payloads are
/// intentionally leaked and never counted.
///
/// # Safety
/// The value pointers must identify valid StyleValueData, and
/// `parent_payload` must identify an SVG reset payload or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_svg_reset_group(
    group_index: usize,
    cx: *const c_void,
    cy: *const c_void,
    r: *const c_void,
    rx: *const c_void,
    ry: *const c_void,
    x: *const c_void,
    y: *const c_void,
    stop_color: u32,
    stop_opacity: f32,
    flood_color: u32,
    flood_opacity: f32,
    vector_effect: *const c_void,
    shape_rendering: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    abort_on_panic(|| {
        let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match unsafe { data.cast::<StyleValueData>().as_ref() } {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let retained = |data: *const c_void| ComputedStyleValueHandle::retained(data.cast());
        let built = SVGResetValues {
            cx: retained(cx),
            cy: retained(cy),
            r: retained(r),
            rx: ComputedLengthPercentageOrAuto::from_data(rx),
            ry: ComputedLengthPercentageOrAuto::from_data(ry),
            x: retained(x),
            y: retained(y),
            stop_color,
            stop_opacity,
            flood_color,
            flood_opacity,
            vector_effect: keyword_code(vector_effect, crate::css::css_enums::keyword_to_vector_effect)?,
            shape_rendering: keyword_code(shape_rendering, crate::css::css_enums::keyword_to_shape_rendering)?,
        };

        if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<SVGResetValues>() }) {
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if built.eq(unsafe { &*default_payload.cast::<SVGResetValues>() }) {
            return Some(default_payload);
        }

        let payload = allocate_payload(vtable(group_index), 1);
        unsafe { payload.cast::<SVGResetValues>().write(built) };
        Some(payload.cast_const())
    })
    .unwrap_or(std::ptr::null())
}

/// Builds the complete surround group from the physical inset, margin, and
/// padding properties. Anchor insets retain their original value separately
/// while exposing auto through the length-box facade, matching layout's
/// existing representation.
///
/// # Safety
/// Each value pointer must address valid StyleValueData, and `parent_payload`
/// must be a valid surround payload or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_surround_group(
    group_index: usize,
    top: *const c_void,
    right: *const c_void,
    bottom: *const c_void,
    left: *const c_void,
    margin_top: *const c_void,
    margin_right: *const c_void,
    margin_bottom: *const c_void,
    margin_left: *const c_void,
    padding_top: *const c_void,
    padding_right: *const c_void,
    padding_bottom: *const c_void,
    padding_left: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    abort_on_panic(|| {
        let anchor = |data: *const c_void| {
            let data = data.cast::<StyleValueData>();
            if matches!(unsafe { data.as_ref() }, Some(StyleValueData::Anchor { .. })) {
                ComputedStyleValueHandle::retained(data)
            } else {
                ComputedStyleValueHandle::empty()
            }
        };
        let built = SurroundValues {
            inset: ComputedLengthBox::from_data(top, right, bottom, left, true),
            top_anchor_inset: anchor(top),
            right_anchor_inset: anchor(right),
            bottom_anchor_inset: anchor(bottom),
            left_anchor_inset: anchor(left),
            margin: ComputedLengthBox::from_data(margin_top, margin_right, margin_bottom, margin_left, false),
            padding: ComputedLengthBox::from_data(padding_top, padding_right, padding_bottom, padding_left, false),
        };

        if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<SurroundValues>() }) {
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if built.eq(unsafe { &*default_payload.cast::<SurroundValues>() }) {
            return Some(default_payload);
        }

        let payload = allocate_payload(vtable(group_index), 1);
        unsafe { payload.cast::<SurroundValues>().write(built) };
        Some(payload.cast_const())
    })
    .unwrap_or(std::ptr::null())
}

/// Builds the complete sizing group from its six computed values. Accepted
/// sizing functions are already constrained to fit-content() by parsing, so
/// the function payload can be consumed without inspecting or copying its
/// interned name.
///
/// # Safety
/// Each value pointer must address valid StyleValueData, and `parent_payload`
/// must be a valid sizing payload or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_sizing_group(
    group_index: usize,
    width: *const c_void,
    min_width: *const c_void,
    max_width: *const c_void,
    height: *const c_void,
    min_height: *const c_void,
    max_height: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    abort_on_panic(|| {
        let built = SizingValues {
            width: ComputedSize::from_data(width),
            min_width: ComputedSize::from_data(min_width),
            max_width: ComputedSize::from_data(max_width),
            height: ComputedSize::from_data(height),
            min_height: ComputedSize::from_data(min_height),
            max_height: ComputedSize::from_data(max_height),
        };

        if !parent_payload.is_null() && built.eq(unsafe { &*(parent_payload as *const SizingValues) }) {
            retain_group_payload(group_index, parent_payload);
            return parent_payload;
        }

        let default_payload = default_group_payload(group_index);
        if built.eq(unsafe { &*(default_payload as *const SizingValues) }) {
            return default_payload;
        }

        let payload = allocate_payload(vtable(group_index), 1);
        unsafe { (payload as *mut SizingValues).write(built) };
        payload
    })
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
    use crate::css::style_value::StyleValueData;

    abort_on_panic(|| {
        let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match unsafe { (data as *const StyleValueData).as_ref() } {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let spacing = match unsafe { (border_spacing as *const StyleValueData).as_ref() } {
            Some(StyleValueData::Length { value, unit }) if *unit == crate::css::style_compute::px_length_unit() => {
                crate::css::css_pixels::CssPixels::nearest_value_for(*value).raw_value()
            }
            _ => return None,
        };
        let built = InheritedTableValues {
            border_collapse: keyword_code(border_collapse, crate::css::style_compute::keyword_to_border_collapse)?,
            caption_side: keyword_code(caption_side, crate::css::style_compute::keyword_to_caption_side)?,
            empty_cells: keyword_code(empty_cells, crate::css::style_compute::keyword_to_empty_cells)?,
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
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InheritedTableValues {
    pub border_collapse: u8,
    pub caption_side: u8,
    pub empty_cells: u8,
    pub border_spacing_horizontal: i32,
    pub border_spacing_vertical: i32,
}

impl InheritedTableValues {
    fn initial() -> Self {
        use crate::css::css_enums::{border_collapse, caption_side, empty_cells};

        Self {
            border_collapse: border_collapse::SEPARATE,
            caption_side: caption_side::TOP,
            empty_cells: empty_cells::SHOW,
            border_spacing_horizontal: 0,
            border_spacing_vertical: 0,
        }
    }
}

impl InheritedBoxValues {
    fn initial() -> Self {
        use crate::css::css_enums::{content_visibility, direction, image_rendering, visibility, writing_mode};

        Self {
            visibility: visibility::VISIBLE,
            direction: direction::LTR,
            writing_mode: writing_mode::HORIZONTAL_TB,
            content_visibility: content_visibility::VISIBLE,
            image_rendering: image_rendering::AUTO,
        }
    }
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

/// Returns the typed view of a sizing group payload.
///
/// # Safety
/// `payload` must be a sizing group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_sizing(payload: *const c_void) -> *const SizingValues {
    payload as *const SizingValues
}

/// Returns the typed view of an alignment group payload.
///
/// # Safety
/// `payload` must be an alignment group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_alignment(payload: *const c_void) -> *const AlignmentValues {
    payload as *const AlignmentValues
}

/// Returns the typed view of an SVG reset group payload.
///
/// # Safety
/// `payload` must be an SVG reset group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_svg_reset(payload: *const c_void) -> *const SVGResetValues {
    payload as *const SVGResetValues
}

/// Returns the typed view of a surround group payload.
///
/// # Safety
/// `payload` must be a surround group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_surround(payload: *const c_void) -> *const SurroundValues {
    payload as *const SurroundValues
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
        let vtables = [
            StyleGroupVTable {
                lifecycle: StyleGroupLifecycle::Cpp,
                size: size_of::<u64>(),
                align: align_of::<u64>(),
                default_construct: Some(test_default_construct),
                copy_construct: Some(test_copy_construct),
                destruct: Some(test_destruct),
                equals: Some(test_equals),
            },
            StyleGroupVTable {
                lifecycle: StyleGroupLifecycle::InheritedTable,
                size: size_of::<InheritedTableValues>(),
                align: align_of::<InheritedTableValues>(),
                default_construct: None,
                copy_construct: None,
                destruct: None,
                equals: None,
            },
            StyleGroupVTable {
                lifecycle: StyleGroupLifecycle::InheritedBox,
                size: size_of::<InheritedBoxValues>(),
                align: align_of::<InheritedBoxValues>(),
                default_construct: None,
                copy_construct: None,
                destruct: None,
                equals: None,
            },
            StyleGroupVTable {
                lifecycle: StyleGroupLifecycle::Sizing,
                size: size_of::<SizingValues>(),
                align: align_of::<SizingValues>(),
                default_construct: None,
                copy_construct: None,
                destruct: None,
                equals: None,
            },
            StyleGroupVTable {
                lifecycle: StyleGroupLifecycle::Alignment,
                size: size_of::<AlignmentValues>(),
                align: align_of::<AlignmentValues>(),
                default_construct: None,
                copy_construct: None,
                destruct: None,
                equals: None,
            },
            StyleGroupVTable {
                lifecycle: StyleGroupLifecycle::SVGReset,
                size: size_of::<SVGResetValues>(),
                align: align_of::<SVGResetValues>(),
                default_construct: None,
                copy_construct: None,
                destruct: None,
                equals: None,
            },
            StyleGroupVTable {
                lifecycle: StyleGroupLifecycle::Surround,
                size: size_of::<SurroundValues>(),
                align: align_of::<SurroundValues>(),
                default_construct: None,
                copy_construct: None,
                destruct: None,
                equals: None,
            },
        ];
        let mut defaults = [std::ptr::null::<c_void>(); 7];
        unsafe {
            rust_style_group_registry_register(vtables.as_ptr(), vtables.len(), defaults.as_mut_ptr());
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

            let table_default = *(defaults[1] as *const InheritedTableValues);
            assert_eq!(table_default, InheritedTableValues::initial());
            let table_clone = rust_style_group_clone(1, defaults[1]);
            assert_eq!(*(table_clone as *const InheritedTableValues), table_default);
            refcount_of(table_clone, align_of::<InheritedTableValues>()).store(0, Ordering::Relaxed);
            rust_style_group_free(1, table_clone);

            let box_default = *(defaults[2] as *const InheritedBoxValues);
            assert_eq!(box_default, InheritedBoxValues::initial());
            let box_clone = rust_style_group_clone(2, defaults[2]);
            assert_eq!(*(box_clone as *const InheritedBoxValues), box_default);
            refcount_of(box_clone, align_of::<InheritedBoxValues>()).store(0, Ordering::Relaxed);
            rust_style_group_free(2, box_clone);

            let sizing_default = &*(defaults[3] as *const SizingValues);
            assert!(sizing_default.eq(&SizingValues::initial()));
            let sizing_clone = rust_style_group_clone(3, defaults[3]);
            assert!((*(sizing_clone as *const SizingValues)).eq(sizing_default));
            refcount_of(sizing_clone, align_of::<SizingValues>()).store(0, Ordering::Relaxed);
            rust_style_group_free(3, sizing_clone);

            let alignment_default = &*(defaults[4] as *const AlignmentValues);
            assert!(alignment_default.eq(&AlignmentValues::initial()));
            let alignment_clone = rust_style_group_clone(4, defaults[4]);
            assert!((*(alignment_clone as *const AlignmentValues)).eq(alignment_default));
            refcount_of(alignment_clone, align_of::<AlignmentValues>()).store(0, Ordering::Relaxed);
            rust_style_group_free(4, alignment_clone);

            let svg_reset_default = &*(defaults[5] as *const SVGResetValues);
            assert!(svg_reset_default.eq(&SVGResetValues::initial()));
            let svg_reset_clone = rust_style_group_clone(5, defaults[5]);
            assert!((*(svg_reset_clone as *const SVGResetValues)).eq(svg_reset_default));
            refcount_of(svg_reset_clone, align_of::<SVGResetValues>()).store(0, Ordering::Relaxed);
            rust_style_group_free(5, svg_reset_clone);

            let surround_default = &*(defaults[6] as *const SurroundValues);
            assert!(surround_default.eq(&SurroundValues::initial()));
            let surround_clone = rust_style_group_clone(6, defaults[6]);
            assert!((*(surround_clone as *const SurroundValues)).eq(surround_default));
            refcount_of(surround_clone, align_of::<SurroundValues>()).store(0, Ordering::Relaxed);
            rust_style_group_free(6, surround_clone);
        }
    }
}
