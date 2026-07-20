/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust-owned CSS style value data.
//!
//! The C++ StyleValue subclasses keep their data in a Rust-owned [`StyleValueData`] allocation
//! instead of C++ member variables. Each subclass owns its allocation uniquely and destroys it
//! with [`rust_style_value_destroy`]. The layout of [`StyleValueData`] is exposed to C++
//! through cbindgen so that hot accessors compile to inline field reads with no FFI call.

use std::ffi::c_void;

use crate::abort_on_panic;

unsafe extern "C" {
    fn ladybird_style_value_unref(style_value: *const c_void);
    fn ladybird_utf16_fly_string_unref(raw: usize);
    fn ladybird_string_unref(raw: usize);
    fn ladybird_calculation_node_unref(node: *const c_void);
    fn ladybird_style_value_ref(style_value: *const c_void);
    fn ladybird_utf16_fly_string_ref(raw: usize);
}

/// A strong reference to a C++ StyleValue held from Rust-owned value data.
///
/// Nested values still point at the C++ shell objects; dropping the Rust allocation releases
/// the reference. Once the shells are collapsed into a single handle type these become
/// references between Rust allocations instead.
#[repr(C)]
pub struct RetainedStyleValue {
    pointer: *const c_void,
}

impl RetainedStyleValue {
    pub(crate) fn shell_pointer(&self) -> *const c_void {
        self.pointer
    }

    /// Assumes ownership of one strong reference to the C++ StyleValue shell.
    ///
    /// # Safety
    /// `pointer` must be a leaked strong StyleValue reference (or null for an absent value).
    pub(crate) unsafe fn from_shell_pointer(pointer: *const c_void) -> Self {
        Self { pointer }
    }

    /// Retains a new strong reference to the C++ StyleValue shell.
    ///
    /// # Safety
    /// `pointer` must point at a live StyleValue shell.
    pub(crate) unsafe fn from_borrowed_shell_pointer(pointer: *const c_void) -> Self {
        unsafe { ladybird_style_value_ref(pointer) };
        Self { pointer }
    }
}

impl Drop for RetainedStyleValue {
    fn drop(&mut self) {
        // A null pointer represents an absent optional reference.
        if !self.pointer.is_null() {
            unsafe { ladybird_style_value_unref(self.pointer) };
        }
    }
}

/// A retained AK::Utf16FlyString, stored as its one-word raw representation. Owns one reference
/// to the underlying string data unless it is a short string, which needs none; the C++ bridge
/// handles both cases.
#[repr(C)]
pub struct RetainedUtf16FlyString {
    raw: usize,
}

impl RetainedUtf16FlyString {
    /// The raw one-word representation; fly strings are interned, so equal raw
    /// values mean equal strings.
    pub(crate) fn raw(&self) -> usize {
        self.raw
    }

    /// Assumes ownership of one reference to the underlying string data.
    ///
    /// # Safety
    /// `raw` must be the raw representation of a fly string whose reference this may own.
    pub(crate) unsafe fn from_raw(raw: usize) -> Self {
        Self { raw }
    }

    /// Retains a new reference to the underlying string data.
    ///
    /// # Safety
    /// `raw` must be the raw representation of a live fly string.
    pub(crate) unsafe fn from_borrowed_raw(raw: usize) -> Self {
        unsafe { ladybird_utf16_fly_string_ref(raw) };
        Self { raw }
    }
}

impl Drop for RetainedUtf16FlyString {
    fn drop(&mut self) {
        unsafe { ladybird_utf16_fly_string_unref(self.raw) };
    }
}

/// Implements `Drop` for a `Retained*List` struct: releases the Rust-owned boxed slice,
/// dropping each element (which releases the element's own retained references).
macro_rules! retained_list_drop {
    ($list:ident) => {
        impl Drop for $list {
            fn drop(&mut self) {
                if !self.pointer.is_null() {
                    drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.pointer, self.length)) });
                }
            }
        }
    };
}

/// Implements the shared behavior for a `Retained*List` struct whose `from_raw` input is an
/// array of its own element type: `from_raw` copies `length` elements into a Rust-owned boxed
/// slice, assuming ownership of the elements' retained references, and `Drop` releases them.
macro_rules! retained_list {
    ($list:ident, $element:ty) => {
        impl $list {
            /// Takes ownership of the elements' retained references.
            ///
            /// # Safety
            /// `elements` must point to `length` valid elements whose retained references this
            /// list may assume ownership of.
            unsafe fn from_raw(elements: *const $element, length: usize) -> Self {
                let slice: Box<[$element]> = (0..length)
                    .map(|i| unsafe { std::ptr::read(elements.add(i)) })
                    .collect();
                let length = slice.len();
                let pointer = Box::into_raw(slice) as *mut $element;
                Self { pointer, length }
            }
        }
        retained_list_drop!($list);
    };
}

/// A retained, Rust-owned array of style value references.
#[repr(C)]
pub struct RetainedStyleValueList {
    pointer: *mut RetainedStyleValue,
    length: usize,
}

impl RetainedStyleValueList {
    pub(crate) fn as_slice(&self) -> &[RetainedStyleValue] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }

    /// Takes ownership of one strong reference to each value.
    ///
    /// # Safety
    /// `values` must point to `length` valid style value pointers.
    unsafe fn from_raw(values: *const *const c_void, length: usize) -> Self {
        let slice: Box<[RetainedStyleValue]> = (0..length)
            .map(|i| RetainedStyleValue {
                pointer: unsafe { *values.add(i) },
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedStyleValue;
        Self { pointer, length }
    }
}

retained_list_drop!(RetainedStyleValueList);

/// A Rust-owned array of C++ PropertyID values (`enum class PropertyID : u16`, opaque to Rust).
#[repr(C)]
pub struct RetainedPropertyIdList {
    pointer: *mut u16,
    length: usize,
}

retained_list!(RetainedPropertyIdList, u16);

impl RetainedPropertyIdList {
    pub(crate) fn as_slice(&self) -> &[u16] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

/// A retained AK::String, stored as its one-word raw representation. Owns one reference to the
/// underlying string data unless it is a short string; the C++ bridge handles both cases.
#[repr(C)]
pub struct RetainedString {
    raw: usize,
}

impl Drop for RetainedString {
    fn drop(&mut self) {
        unsafe { ladybird_string_unref(self.raw) };
    }
}

/// A retained CSS request URL modifier: the modifier type and either an enum value or a retained
/// string value (raw 0 when the value is an enum). All enums are C++ `enum class ... : u8`
/// values, opaque to Rust.
#[repr(C)]
pub struct RetainedRequestUrlModifier {
    modifier_type: u8,
    enum_value: u8,
    string_value: RetainedUtf16FlyString,
}

/// A Rust-owned array of retained request URL modifiers.
#[repr(C)]
pub struct RetainedRequestUrlModifierList {
    pointer: *mut RetainedRequestUrlModifier,
    length: usize,
}

retained_list!(RetainedRequestUrlModifierList, RetainedRequestUrlModifier);

/// A Rust-owned array of bytes, used for lists of C++ u8 enum values.
#[repr(C)]
pub struct RetainedByteList {
    pointer: *mut u8,
    length: usize,
}

retained_list!(RetainedByteList, u8);

/// A Rust-owned array of retained AK::Utf16FlyString values.
#[repr(C)]
pub struct RetainedUtf16FlyStringList {
    pointer: *mut RetainedUtf16FlyString,
    length: usize,
}

impl RetainedUtf16FlyStringList {
    /// Takes ownership of one leaked reference to each string.
    ///
    /// # Safety
    /// `strings` must point to `length` valid leaked string raws.
    unsafe fn from_raw(strings: *const usize, length: usize) -> Self {
        let slice: Box<[RetainedUtf16FlyString]> = (0..length)
            .map(|i| RetainedUtf16FlyString {
                raw: unsafe { *strings.add(i) },
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedUtf16FlyString;
        Self { pointer, length }
    }
}

retained_list_drop!(RetainedUtf16FlyStringList);

/// A retained counter definition: the counter name, the reversed flag and an optional retained
/// value (null when absent).
#[repr(C)]
pub struct RetainedCounterDefinition {
    name: RetainedUtf16FlyString,
    is_reversed: bool,
    value: RetainedStyleValue,
}

/// A Rust-owned array of retained counter definitions.
#[repr(C)]
pub struct RetainedCounterDefinitionList {
    pointer: *mut RetainedCounterDefinition,
    length: usize,
}

retained_list!(RetainedCounterDefinitionList, RetainedCounterDefinition);

/// A retained image-set() option: the image, its resolution and an optional type string (a
/// retained AK::Utf16String raw, 0 when absent, released through the same bridge as fly
/// strings).
#[repr(C)]
pub struct RetainedImageSetOption {
    image: RetainedStyleValue,
    resolution: RetainedStyleValue,
    has_type: bool,
    type_string: RetainedUtf16FlyString,
}

/// A Rust-owned array of retained image-set() options.
#[repr(C)]
pub struct RetainedImageSetOptionList {
    pointer: *mut RetainedImageSetOption,
    length: usize,
}

retained_list!(RetainedImageSetOptionList, RetainedImageSetOption);

/// A retained gradient color stop: an optional transition hint, then an optional color,
/// position and second position (each null when absent). The layout matches the C++
/// ColorStopListElement, which is four reference pointers, so C++ views these in place.
#[repr(C)]
pub struct RetainedColorStop {
    transition_hint: RetainedStyleValue,
    color: RetainedStyleValue,
    position: RetainedStyleValue,
    second_position: RetainedStyleValue,
}

/// A Rust-owned array of retained gradient color stops.
#[repr(C)]
pub struct RetainedColorStopList {
    pointer: *mut RetainedColorStop,
    length: usize,
}

retained_list!(RetainedColorStopList, RetainedColorStop);

impl RetainedCounterDefinition {
    pub(crate) fn value(&self) -> &RetainedStyleValue {
        &self.value
    }
}

impl RetainedImageSetOption {
    pub(crate) fn values(&self) -> [&RetainedStyleValue; 2] {
        [&self.image, &self.resolution]
    }
}

impl RetainedLinearEasingStop {
    pub(crate) fn values(&self) -> [&RetainedStyleValue; 2] {
        [&self.output, &self.input]
    }
}

macro_rules! retained_list_as_slice {
    ($list:ident, $element:ty) => {
        impl $list {
            pub(crate) fn as_slice(&self) -> &[$element] {
                if self.pointer.is_null() {
                    return &[];
                }
                unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
            }
        }
    };
}
retained_list_as_slice!(RetainedCounterDefinitionList, RetainedCounterDefinition);
retained_list_as_slice!(RetainedImageSetOptionList, RetainedImageSetOption);
retained_list_as_slice!(RetainedLinearEasingStopList, RetainedLinearEasingStop);

impl RetainedShapePoint {
    pub(crate) fn values(&self) -> [&RetainedStyleValue; 2] {
        [&self.x, &self.y]
    }
}
retained_list_as_slice!(RetainedShapePointList, RetainedShapePoint);

impl RetainedColorStop {
    /// The stop's retained values, absent ones as null retained references.
    pub(crate) fn values(&self) -> [&RetainedStyleValue; 4] {
        [
            &self.transition_hint,
            &self.color,
            &self.position,
            &self.second_position,
        ]
    }
}

impl RetainedColorStopList {
    pub(crate) fn as_slice(&self) -> &[RetainedColorStop] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

/// A retained named grid area: the retained area name and its grid line indices.
#[repr(C)]
pub struct RetainedGridArea {
    name: RetainedUtf16FlyString,
    row_start: usize,
    row_end: usize,
    column_start: usize,
    column_end: usize,
}

/// A Rust-owned array of retained named grid areas.
#[repr(C)]
pub struct RetainedGridAreaList {
    pointer: *mut RetainedGridArea,
    length: usize,
}

retained_list!(RetainedGridAreaList, RetainedGridArea);

/// A retained linear() easing stop: the output value and an optional input (null when absent).
#[repr(C)]
pub struct RetainedLinearEasingStop {
    output: RetainedStyleValue,
    input: RetainedStyleValue,
}

/// A Rust-owned array of retained linear() easing stops.
#[repr(C)]
pub struct RetainedLinearEasingStopList {
    pointer: *mut RetainedLinearEasingStop,
    length: usize,
}

retained_list!(RetainedLinearEasingStopList, RetainedLinearEasingStop);

/// Borrowed input description of one grid track list entry, used when creating a grid track
/// size list. Kinds: 0 = line names, 1 = a single size, 2 = minmax, 3 = repeat with a nested
/// entry list.
#[repr(C)]
pub struct GridTrackEntryInput {
    kind: u8,
    names: *const usize,
    name_count: usize,
    size_value: *const c_void,
    min_value: *const c_void,
    max_value: *const c_void,
    repeat_type: u8,
    repeat_count: *const c_void,
    repeat_is_subgrid: bool,
    repeat_preserve_line_name_sets: bool,
    repeat_entries: *const GridTrackEntryInput,
    repeat_entry_count: usize,
}

/// A Rust-owned array of retained grid track list entries.
#[repr(C)]
pub struct RetainedGridTrackEntryList {
    pointer: *mut RetainedGridTrackEntry,
    length: usize,
}

/// A retained, Rust-owned grid track list entry (see [`GridTrackEntryInput`] for the kinds).
#[repr(C)]
pub struct RetainedGridTrackEntry {
    kind: u8,
    names: RetainedUtf16FlyStringList,
    size_value: RetainedStyleValue,
    min_value: RetainedStyleValue,
    max_value: RetainedStyleValue,
    repeat_type: u8,
    repeat_count: RetainedStyleValue,
    repeat_is_subgrid: bool,
    repeat_preserve_line_name_sets: bool,
    repeat_entries_pointer: *mut RetainedGridTrackEntry,
    repeat_entries_length: usize,
}

impl Drop for RetainedGridTrackEntry {
    fn drop(&mut self) {
        if !self.repeat_entries_pointer.is_null() {
            drop(unsafe {
                Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                    self.repeat_entries_pointer,
                    self.repeat_entries_length,
                ))
            });
        }
    }
}

impl RetainedGridTrackEntryList {
    /// Takes ownership of the entries' retained values and names, recursively for nested
    /// repeat lists.
    ///
    /// # Safety
    /// `entries` must point to `length` valid entry descriptions.
    unsafe fn from_raw(entries: *const GridTrackEntryInput, length: usize) -> Self {
        let slice: Box<[RetainedGridTrackEntry]> = (0..length)
            .map(|i| {
                let input = unsafe { &*entries.add(i) };
                RetainedGridTrackEntry {
                    kind: input.kind,
                    names: unsafe { RetainedUtf16FlyStringList::from_raw(input.names, input.name_count) },
                    size_value: RetainedStyleValue {
                        pointer: input.size_value,
                    },
                    min_value: RetainedStyleValue {
                        pointer: input.min_value,
                    },
                    max_value: RetainedStyleValue {
                        pointer: input.max_value,
                    },
                    repeat_type: input.repeat_type,
                    repeat_count: RetainedStyleValue {
                        pointer: input.repeat_count,
                    },
                    repeat_is_subgrid: input.repeat_is_subgrid,
                    repeat_preserve_line_name_sets: input.repeat_preserve_line_name_sets,
                    repeat_entries_pointer: {
                        let nested = unsafe {
                            RetainedGridTrackEntryList::from_raw(input.repeat_entries, input.repeat_entry_count)
                        };
                        let pointer = nested.pointer;
                        std::mem::forget(nested);
                        pointer
                    },
                    repeat_entries_length: input.repeat_entry_count,
                }
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedGridTrackEntry;
        Self { pointer, length }
    }
}

retained_list_drop!(RetainedGridTrackEntryList);

/// A retained polygon point: the x and y style values.
#[repr(C)]
pub struct RetainedShapePoint {
    x: RetainedStyleValue,
    y: RetainedStyleValue,
}

/// A Rust-owned array of retained polygon points.
#[repr(C)]
pub struct RetainedShapePointList {
    pointer: *mut RetainedShapePoint,
    length: usize,
}

retained_list!(RetainedShapePointList, RetainedShapePoint);

/// A strong reference to a C++ CalculationNode held from Rust-owned value data. The node tree
/// is still C++-structured during the migration, like retained style values.
#[repr(C)]
pub struct RetainedCalculationNode {
    pointer: *const c_void,
}

impl Drop for RetainedCalculationNode {
    fn drop(&mut self) {
        if !self.pointer.is_null() {
            unsafe { ladybird_calculation_node_unref(self.pointer) };
        }
    }
}

/// An accepted numeric range for one value type (the C++ `enum class ValueType : u8`, opaque
/// to Rust).
#[repr(C)]
pub struct RetainedNumericRangeByType {
    value_type: u8,
    min: f64,
    max: f64,
}

#[allow(dead_code)]
impl RetainedNumericRangeByType {
    pub(crate) fn value_type(&self) -> u8 {
        self.value_type
    }

    pub(crate) fn range(&self) -> (f64, f64) {
        (self.min, self.max)
    }
}

/// A Rust-owned array of accepted numeric ranges.
#[repr(C)]
pub struct RetainedNumericRangeList {
    pointer: *mut RetainedNumericRangeByType,
    length: usize,
}

retained_list!(RetainedNumericRangeList, RetainedNumericRangeByType);

#[allow(dead_code)]
impl RetainedNumericRangeList {
    pub(crate) fn as_slice(&self) -> &[RetainedNumericRangeByType] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

/// The shared leading fields of every color variant payload: the optional color type and the
/// color syntax. Placing this first in each color payload lets C++ read it without knowing
/// which color variant it has.
#[repr(C)]
pub struct ColorBase {
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
}

/// The data of a single immutable CSS style value.
///
/// Variant payload fields are read directly by the corresponding C++ StyleValue subclass, so
/// changing a payload changes the C++ accessors reading it.
#[repr(C, u8)]
// NB: Variant payload fields are only read by C++ through the exposed layout.
#[allow(dead_code)]
pub enum StyleValueData {
    /// A CSS keyword. The value is the generated C++ `enum class Keyword : u16`, opaque to Rust.
    Keyword { keyword: u16 },
    /// A CSS `<number>`.
    Number { value: f64 },
    /// A CSS `<integer>`.
    Integer { value: i32 },
    /// A CSS `<angle>`. The unit is the C++ `enum class AngleUnit : u8`, opaque to Rust.
    Angle { value: f64, unit: u8 },
    /// A CSS `<flex>`. The unit is the C++ `enum class FlexUnit : u8`, opaque to Rust.
    Flex { value: f64, unit: u8 },
    /// A CSS `<frequency>`. The unit is the C++ `enum class FrequencyUnit : u8`, opaque to Rust.
    Frequency { value: f64, unit: u8 },
    /// A CSS `<length>`. The unit is the C++ `enum class LengthUnit : u8`, opaque to Rust.
    Length { value: f64, unit: u8 },
    /// A CSS `<percentage>`.
    Percentage { value: f64 },
    /// A CSS `<resolution>`. The unit is the C++ `enum class ResolutionUnit : u8`, opaque to Rust.
    Resolution { value: f64, unit: u8 },
    /// A CSS `<time>`. The unit is the C++ `enum class TimeUnit : u8`, opaque to Rust.
    Time { value: f64, unit: u8 },
    /// A basic shape. Kinds: inset (0), xywh (1) and rect (2) use five retained value slots;
    /// circle (3) and ellipse (4) use two; polygon (5) uses the fill rule and points; path (6)
    /// uses the fill rule and the retained serialized path data string.
    BasicShape {
        kind: u8,
        v0: RetainedStyleValue,
        v1: RetainedStyleValue,
        v2: RetainedStyleValue,
        v3: RetainedStyleValue,
        v4: RetainedStyleValue,
        fill_rule: u8,
        points: RetainedShapePointList,
        path_string: RetainedUtf16FlyString,
    },
    /// A calc() or other math function: the retained calculation node tree root, the resolved
    /// numeric type as its raw bytes (a trivially copyable C++ NumericType, opaque to Rust)
    /// and the parse-time calculation context.
    Calculated {
        calculation: RetainedCalculationNode,
        rust_calculation: crate::calc::CalcNodeHandle,
        /// The resolve-against target, base-mapped at creation: whether one
        /// exists, whether it is the number type, and otherwise its base type
        /// index in the numeric type order.
        resolve_as_is_number: bool,
        resolve_as_base: u8,
        resolved_type: RetainedByteList,
        has_percentages_resolve_as: bool,
        percentages_resolve_as: u8,
        resolve_numbers_as_integers: bool,
        accepted_ranges: RetainedNumericRangeList,
    },
    /// A CSS `<ratio>`, e.g. `16 / 9`. The numerator and denominator are style values.
    Ratio {
        numerator: RetainedStyleValue,
        denominator: RetainedStyleValue,
    },
    /// A unicode-range, e.g. `U+0025-00FF`.
    UnicodeRange { min_code_point: u32, max_code_point: u32 },
    /// A CSS `<opacity-value>`: a number, percentage or calculated style value.
    OpacityValue { value: RetainedStyleValue },
    /// One edge of a CSS `<position>`: an optional edge keyword (the C++ `enum class
    /// PositionEdge : u8`, opaque to Rust) and an optional offset style value (null when absent).
    Edge {
        has_edge: bool,
        edge: u8,
        offset: RetainedStyleValue,
    },
    /// The guaranteed-invalid value: https://drafts.csswg.org/css-variables/#guaranteed-invalid-value
    GuaranteedInvalid,
    /// An absent optional value in a shorthand.
    EmptyOptional,
    /// grid-auto-flow.
    GridAutoFlow { row: bool, dense: bool },
    /// text-underline-position. Both fields are C++ `enum class ... : u8` values, opaque to Rust.
    TextUnderlinePosition { horizontal: u8, vertical: u8 },
    /// contrast-color() with its retained color style value.
    ContrastColor {
        color_base: ColorBase,
        color: RetainedStyleValue,
    },
    /// superellipse() with its retained parameter style value.
    Superellipse { parameter: RetainedStyleValue },
    /// A pending-substitution value retaining the shorthand value it came from.
    PendingSubstitution {
        original_shorthand_value: RetainedStyleValue,
    },
    /// scrollbar-color with retained thumb and track color values.
    ScrollbarColor {
        thumb_color: RetainedStyleValue,
        track_color: RetainedStyleValue,
    },
    /// rect() with four retained edge style values.
    Rect {
        top: RetainedStyleValue,
        right: RetainedStyleValue,
        bottom: RetainedStyleValue,
        left: RetainedStyleValue,
    },
    /// A CSS `<string>`.
    String { string: RetainedUtf16FlyString },
    /// An unrecognized CSS function, kept as its name and argument value.
    Function {
        name: RetainedUtf16FlyString,
        value: RetainedStyleValue,
    },
    /// An OpenType tag with its value, from font-feature-settings or font-variation-settings.
    /// The mode is the C++ OpenTypeTaggedStyleValue::Mode, opaque to Rust.
    OpenTypeTagged {
        mode: u8,
        tag: RetainedUtf16FlyString,
        value: RetainedStyleValue,
    },
    /// font-style: a keyword (the C++ `enum class FontStyleKeyword : u8`, opaque to Rust) and
    /// an optional oblique angle style value (null when absent).
    FontStyle {
        font_style: u8,
        angle_value: RetainedStyleValue,
    },
    /// text-indent: a length-percentage style value plus the hanging and each-line flags.
    TextIndent {
        length_percentage: RetainedStyleValue,
        hanging: bool,
        each_line: bool,
    },
    /// overflow-clip-margin: an optional visual box (the C++ `enum class BackgroundBox : u8`,
    /// opaque to Rust) and an offset style value.
    OverflowClipMargin {
        has_visual_box: bool,
        visual_box: u8,
        offset: RetainedStyleValue,
    },
    /// sibling-count() or sibling-index(). Both fields are C++ `enum class ... : u8` values,
    /// opaque to Rust.
    TreeCountingFunction { function: u8, computed_type: u8 },
    /// background-size with its two retained size style values.
    BackgroundSize {
        size_x: RetainedStyleValue,
        size_y: RetainedStyleValue,
    },
    /// A background repeat-style. Both fields are the C++ `enum class Repetition : u8`, opaque
    /// to Rust.
    RepeatStyle { repeat_x: u8, repeat_y: u8 },
    /// border-image-slice: four retained offset style values and the fill keyword.
    BorderImageSlice {
        top: RetainedStyleValue,
        right: RetainedStyleValue,
        bottom: RetainedStyleValue,
        left: RetainedStyleValue,
        fill: bool,
    },
    /// anchor-size(): an optional anchor name, an optional size keyword (the C++ `enum class
    /// AnchorSize : u8`, opaque to Rust) and an optional retained fallback value.
    AnchorSize {
        has_anchor_name: bool,
        anchor_name: RetainedUtf16FlyString,
        has_anchor_size: bool,
        anchor_size: u8,
        fallback_value: RetainedStyleValue,
    },
    /// anchor(): an optional anchor name, the retained side style value and an optional
    /// retained fallback value.
    Anchor {
        has_anchor_name: bool,
        anchor_name: RetainedUtf16FlyString,
        anchor_side: RetainedStyleValue,
        fallback_value: RetainedStyleValue,
    },
    /// A CSS `<position>` with its two retained edge style values.
    Position {
        edge_x: RetainedStyleValue,
        edge_y: RetainedStyleValue,
    },
    /// A shadow. The type and placement are C++ enums, opaque to Rust; the color, blur radius
    /// and spread distance are optional retained style values (null when absent).
    Shadow {
        shadow_type: u8,
        color: RetainedStyleValue,
        offset_x: RetainedStyleValue,
        offset_y: RetainedStyleValue,
        blur_radius: RetainedStyleValue,
        spread_distance: RetainedStyleValue,
        placement: u8,
    },
    /// content with its retained content list and optional alt-text list (null when absent).
    Content {
        content: RetainedStyleValue,
        alt_text: RetainedStyleValue,
    },
    /// A @counter-style system descriptor: a plain system keyword (kind 0, the C++ `enum class
    /// CounterStyleSystem : u8`, opaque to Rust), fixed with an optional retained first symbol
    /// (kind 1), or extends with a retained counter style name (kind 2).
    CounterStyleSystem {
        kind: u8,
        system: u8,
        first_symbol: RetainedStyleValue,
        name: RetainedUtf16FlyString,
    },
    /// A counter style reference: either a retained counter style name, or a symbols() function
    /// with its type (the C++ `enum class SymbolsType : u8`, opaque to Rust) and retained
    /// symbol strings.
    CounterStyle {
        is_symbols: bool,
        name: RetainedUtf16FlyString,
        symbols_type: u8,
        symbols: RetainedUtf16FlyStringList,
    },
    /// A color function such as rgb() or oklch(): three retained channel values, an optional
    /// retained alpha, an optional name and an optional retained origin color for relative
    /// color syntax.
    ColorFunction {
        color_base: ColorBase,
        channel_0: RetainedStyleValue,
        channel_1: RetainedStyleValue,
        channel_2: RetainedStyleValue,
        alpha: RetainedStyleValue,
        has_name: bool,
        name: RetainedUtf16FlyString,
        origin_color: RetainedStyleValue,
    },
    /// color-mix() with its optional retained interpolation method value and two components,
    /// each a retained color with an optional retained percentage.
    ColorMix {
        color_base: ColorBase,
        color_interpolation_method: RetainedStyleValue,
        first_color: RetainedStyleValue,
        first_percentage: RetainedStyleValue,
        second_color: RetainedStyleValue,
        second_percentage: RetainedStyleValue,
    },
    /// The shared data of every color style value: an optional color type and the color syntax
    /// (both C++ enums on ColorStyleValue, opaque to Rust).
    /// linear-gradient(): a direction (either a retained angle value or a side-or-corner
    /// keyword), the retained color stops, the gradient type, the repeating flag, an optional
    /// retained interpolation method and the color syntax. Enums are C++ types, opaque to Rust.
    LinearGradient {
        has_direction_value: bool,
        direction_value: RetainedStyleValue,
        side_or_corner: u8,
        color_stop_list: RetainedColorStopList,
        gradient_type: u8,
        repeating: bool,
        color_interpolation_method: RetainedStyleValue,
        color_syntax: u8,
    },
    /// conic-gradient(): an optional retained from-angle, the retained position, the retained
    /// color stops, the repeating flag, an optional retained interpolation method and the color
    /// syntax (a C++ enum, opaque to Rust).
    ConicGradient {
        from_angle: RetainedStyleValue,
        position: RetainedStyleValue,
        color_stop_list: RetainedColorStopList,
        repeating: bool,
        color_interpolation_method: RetainedStyleValue,
        color_syntax: u8,
    },
    /// radial-gradient(): the ending shape (a C++ enum, opaque to Rust), the retained size and
    /// position values, the retained color stops, the repeating flag, an optional retained
    /// interpolation method and the color syntax.
    RadialGradient {
        ending_shape: u8,
        size: RetainedStyleValue,
        position: RetainedStyleValue,
        color_stop_list: RetainedColorStopList,
        repeating: bool,
        color_interpolation_method: RetainedStyleValue,
        color_syntax: u8,
    },
    /// A url() image. Only the CSS URL is immutable value data; the style sheet attachment and
    /// loading state stay on the C++ side.
    Image {
        url: RetainedString,
        url_type: u8,
        url_modifiers: RetainedRequestUrlModifierList,
    },
    /// image-set() with its retained options.
    ImageSet { options: RetainedImageSetOptionList },
    /// An easing function: linear() with its retained stops (kind 0), cubic-bezier() with four
    /// retained control values (kind 1), or steps() with a retained interval count and a step
    /// position (kind 2, the C++ `enum class StepPosition : u8`, opaque to Rust).
    Easing {
        kind: u8,
        linear_stops: RetainedLinearEasingStopList,
        x1: RetainedStyleValue,
        y1: RetainedStyleValue,
        x2: RetainedStyleValue,
        y2: RetainedStyleValue,
        number_of_intervals: RetainedStyleValue,
        step_position: u8,
    },
    /// A cursor with its retained image value and optional retained hotspot coordinates (both
    /// null or both non-null).
    Cursor {
        image: RetainedStyleValue,
        x: RetainedStyleValue,
        y: RetainedStyleValue,
    },
    /// A grid track size list: the subgrid and preserve-line-name-sets flags and the retained
    /// track entries.
    GridTrackSizeList {
        is_subgrid: bool,
        preserve_line_name_sets: bool,
        entries: RetainedGridTrackEntryList,
    },
    /// grid-template-areas with its retained named areas and the row and column counts.
    GridTemplateArea {
        grid_areas: RetainedGridAreaList,
        row_count: usize,
        column_count: usize,
    },
    /// counter-increment, counter-reset or counter-set with its retained counter definitions.
    CounterDefinitions {
        counter_definitions: RetainedCounterDefinitionList,
    },
    /// A grid-row/grid-column placement: auto (0), a span (1) or an area/line (2), with an
    /// optional retained line value and an optional retained name.
    GridTrackPlacement {
        kind: u8,
        value: RetainedStyleValue,
        has_name: bool,
        name: RetainedUtf16FlyString,
    },
    /// counter() or counters(). The function is the C++ CounterFunction enum, opaque to Rust;
    /// the join string is empty for counter().
    Counter {
        function: u8,
        counter_name: RetainedUtf16FlyString,
        counter_style: RetainedStyleValue,
        join_string: RetainedUtf16FlyString,
    },
    /// light-dark() with its two retained color style values.
    LightDark {
        color_base: ColorBase,
        light: RetainedStyleValue,
        dark: RetainedStyleValue,
    },
    /// random-value-sharing: an optional retained fixed value (null when absent), the auto flag,
    /// an optional name and the element-shared flag.
    RandomValueSharing {
        fixed_value: RetainedStyleValue,
        is_auto: bool,
        has_name: bool,
        name: RetainedUtf16FlyString,
        element_shared: bool,
    },
    /// scrollbar-gutter. The value is the C++ `enum class ScrollbarGutter : u8`, opaque to Rust.
    ScrollbarGutter { value: u8 },
    /// A color interpolation method: either a rectangular color space, or a polar color space
    /// with a hue interpolation method. All fields are C++ `enum class ... : u8` values, opaque
    /// to Rust.
    ColorInterpolationMethod {
        is_polar: bool,
        color_space: u8,
        hue_interpolation_method: u8,
    },
    /// A list of style values. The separator and collapsible flag come from the C++
    /// StyleValueList enums, opaque to Rust.
    ValueList {
        values: RetainedStyleValueList,
        separator: u8,
        collapsible: bool,
    },
    /// A tuple of optional style values (null entries represent absent optionals).
    Tuple { values: RetainedStyleValueList },
    /// A display value: the raw bytes of the C++ Display value type (a tag plus a union of
    /// packed u8 enums), opaque to Rust.
    Display { raw: u32 },
    /// color-scheme with its retained scheme names and the only keyword flag.
    ColorScheme {
        schemes: RetainedUtf16FlyStringList,
        only: bool,
    },
    /// An unresolved value containing arbitrary substitution functions, kept as its retained
    /// source text, an optional normalized comparison text (empty when absent), the presence
    /// flags of each substitution function and the attr-taint flag.
    Unresolved {
        source_text: RetainedString,
        value_comparison_text: RetainedString,
        presence_attr: bool,
        presence_dashed_function: bool,
        presence_env: bool,
        presence_if: bool,
        presence_inherit: bool,
        presence_var: bool,
        contains_attr_tainted_values: bool,
    },
    /// A CSS url() or src() with its retained URL string, type (the C++ URL::Type, opaque to
    /// Rust) and request URL modifiers.
    Url {
        url: RetainedString,
        url_type: u8,
        modifiers: RetainedRequestUrlModifierList,
    },
    /// A @font-face source: either local() with a retained family name value, or a URL (encoded
    /// as in [`StyleValueData::Url`]) with an optional format string and a list of font
    /// technologies (C++ `enum class FontTech : u8`, opaque to Rust).
    FontSource {
        is_local: bool,
        local_name: RetainedStyleValue,
        url: RetainedString,
        url_type: u8,
        url_modifiers: RetainedRequestUrlModifierList,
        has_format: bool,
        format: RetainedUtf16FlyString,
        tech: RetainedByteList,
    },
    /// A radial gradient size: one or two components, each either a RadialExtent keyword (the
    /// C++ `enum class RadialExtent : u8`, opaque to Rust) or a retained style value.
    RadialSize {
        component_count: u8,
        is_extent_0: bool,
        extent_0: u8,
        value_0: RetainedStyleValue,
        is_extent_1: bool,
        extent_1: u8,
        value_1: RetainedStyleValue,
    },
    /// A transform function with its argument values. The property (PropertyID : u16) and
    /// function are C++ enums, opaque to Rust.
    Transformation {
        property: u16,
        transform_function: u8,
        values: RetainedStyleValueList,
    },
    /// A shorthand property value: the shorthand id, its longhand ids (both C++
    /// `enum class PropertyID : u16`, opaque to Rust) and their values.
    Shorthand {
        shorthand_property: u16,
        sub_properties: RetainedPropertyIdList,
        values: RetainedStyleValueList,
    },
    /// A CSS `<custom-ident>`.
    CustomIdent { custom_ident: RetainedUtf16FlyString },
    /// A border-radius rect of four retained corner radius style values.
    BorderRadiusRect {
        top_left: RetainedStyleValue,
        top_right: RetainedStyleValue,
        bottom_right: RetainedStyleValue,
        bottom_left: RetainedStyleValue,
    },
    /// A single corner radius: the horizontal and vertical radii and whether they differ.
    BorderRadius {
        is_elliptical: bool,
        horizontal_radius: RetainedStyleValue,
        vertical_radius: RetainedStyleValue,
    },
    /// A filter function. Kinds: blur (0, value = radius), drop-shadow (1, value = shadow),
    /// hue-rotate (2, value = angle), color (3, value = amount, with the color operation).
    /// The kind and the color operation are C++ enum values, opaque to Rust.
    Filter {
        kind: u8,
        color_operation: u8,
        value: RetainedStyleValue,
    },
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_keyword(keyword: u16) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Keyword { keyword })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_number(value: f64) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Number { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_integer(value: i32) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Integer { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_angle(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Angle { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_flex(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Flex { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_frequency(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Frequency { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_length(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Length { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_percentage(value: f64) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Percentage { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_resolution(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Resolution { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_time(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Time { value, unit })))
}

/// Takes ownership of one strong reference to each of the numerator and denominator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_ratio(
    numerator: *const c_void,
    denominator: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Ratio {
            numerator: RetainedStyleValue { pointer: numerator },
            denominator: RetainedStyleValue { pointer: denominator },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_unicode_range(
    min_code_point: u32,
    max_code_point: u32,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::UnicodeRange {
            min_code_point,
            max_code_point,
        }))
    })
}

/// Takes ownership of one strong reference to the value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_opacity_value(value: *const c_void) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::OpacityValue {
            value: RetainedStyleValue { pointer: value },
        }))
    })
}

/// Takes ownership of one strong reference to the offset if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_edge(
    has_edge: bool,
    edge: u8,
    offset: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Edge {
            has_edge,
            edge,
            offset: RetainedStyleValue { pointer: offset },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_guaranteed_invalid() -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::GuaranteedInvalid)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_empty_optional() -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::EmptyOptional)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_grid_auto_flow(row: bool, dense: bool) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::GridAutoFlow { row, dense })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_text_underline_position(horizontal: u8, vertical: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::TextUnderlinePosition { horizontal, vertical })))
}

/// Takes ownership of one strong reference to the color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_contrast_color(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    color: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ContrastColor {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            color: RetainedStyleValue { pointer: color },
        }))
    })
}

/// Takes ownership of one strong reference to the parameter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_superellipse(parameter: *const c_void) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Superellipse {
            parameter: RetainedStyleValue { pointer: parameter },
        }))
    })
}

/// Takes ownership of one strong reference to the original shorthand value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_pending_substitution(
    original_shorthand_value: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::PendingSubstitution {
            original_shorthand_value: RetainedStyleValue {
                pointer: original_shorthand_value,
            },
        }))
    })
}

/// Takes ownership of one strong reference to each color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_scrollbar_color(
    thumb_color: *const c_void,
    track_color: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ScrollbarColor {
            thumb_color: RetainedStyleValue { pointer: thumb_color },
            track_color: RetainedStyleValue { pointer: track_color },
        }))
    })
}

/// Takes ownership of one strong reference to each edge.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_rect(
    top: *const c_void,
    right: *const c_void,
    bottom: *const c_void,
    left: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Rect {
            top: RetainedStyleValue { pointer: top },
            right: RetainedStyleValue { pointer: right },
            bottom: RetainedStyleValue { pointer: bottom },
            left: RetainedStyleValue { pointer: left },
        }))
    })
}

/// Takes ownership of one strong reference to the filter's value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_filter(
    kind: u8,
    color_operation: u8,
    value: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Filter {
            kind,
            color_operation,
            value: RetainedStyleValue { pointer: value },
        }))
    })
}

/// Takes ownership of one strong reference to each radius.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_radius(
    is_elliptical: bool,
    horizontal_radius: *const c_void,
    vertical_radius: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::BorderRadius {
            is_elliptical,
            horizontal_radius: RetainedStyleValue {
                pointer: horizontal_radius,
            },
            vertical_radius: RetainedStyleValue {
                pointer: vertical_radius,
            },
        }))
    })
}

/// Takes ownership of one strong reference to each corner radius.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_radius_rect(
    top_left: *const c_void,
    top_right: *const c_void,
    bottom_right: *const c_void,
    bottom_left: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::BorderRadiusRect {
            top_left: RetainedStyleValue { pointer: top_left },
            top_right: RetainedStyleValue { pointer: top_right },
            bottom_right: RetainedStyleValue { pointer: bottom_right },
            bottom_left: RetainedStyleValue { pointer: bottom_left },
        }))
    })
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_string(string: usize) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::String {
            string: RetainedUtf16FlyString { raw: string },
        }))
    })
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_custom_ident(custom_ident: usize) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::CustomIdent {
            custom_ident: RetainedUtf16FlyString { raw: custom_ident },
        }))
    })
}

/// Takes ownership of one leaked reference to the name and one strong reference to the value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_function(name: usize, value: *const c_void) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Function {
            name: RetainedUtf16FlyString { raw: name },
            value: RetainedStyleValue { pointer: value },
        }))
    })
}

/// Takes ownership of one leaked reference to the tag and one strong reference to the value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_open_type_tagged(
    mode: u8,
    tag: usize,
    value: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::OpenTypeTagged {
            mode,
            tag: RetainedUtf16FlyString { raw: tag },
            value: RetainedStyleValue { pointer: value },
        }))
    })
}

/// Takes ownership of one strong reference to the angle value if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_font_style(
    font_style: u8,
    angle_value: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::FontStyle {
            font_style,
            angle_value: RetainedStyleValue { pointer: angle_value },
        }))
    })
}

/// Takes ownership of one strong reference to the length-percentage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_text_indent(
    length_percentage: *const c_void,
    hanging: bool,
    each_line: bool,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::TextIndent {
            length_percentage: RetainedStyleValue {
                pointer: length_percentage,
            },
            hanging,
            each_line,
        }))
    })
}

/// Takes ownership of one strong reference to the offset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_overflow_clip_margin(
    has_visual_box: bool,
    visual_box: u8,
    offset: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::OverflowClipMargin {
            has_visual_box,
            visual_box,
            offset: RetainedStyleValue { pointer: offset },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_tree_counting_function(
    function: u8,
    computed_type: u8,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::TreeCountingFunction {
            function,
            computed_type,
        }))
    })
}

/// Takes ownership of one strong reference to each size.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_background_size(
    size_x: *const c_void,
    size_y: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::BackgroundSize {
            size_x: RetainedStyleValue { pointer: size_x },
            size_y: RetainedStyleValue { pointer: size_y },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_repeat_style(repeat_x: u8, repeat_y: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::RepeatStyle { repeat_x, repeat_y })))
}

/// Takes ownership of one strong reference to each offset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_image_slice(
    top: *const c_void,
    right: *const c_void,
    bottom: *const c_void,
    left: *const c_void,
    fill: bool,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::BorderImageSlice {
            top: RetainedStyleValue { pointer: top },
            right: RetainedStyleValue { pointer: right },
            bottom: RetainedStyleValue { pointer: bottom },
            left: RetainedStyleValue { pointer: left },
            fill,
        }))
    })
}

/// Takes ownership of one leaked reference to the anchor name (0 when absent) and one strong
/// reference to the fallback value if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_anchor_size(
    has_anchor_name: bool,
    anchor_name: usize,
    has_anchor_size: bool,
    anchor_size: u8,
    fallback_value: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::AnchorSize {
            has_anchor_name,
            anchor_name: RetainedUtf16FlyString { raw: anchor_name },
            has_anchor_size,
            anchor_size,
            fallback_value: RetainedStyleValue {
                pointer: fallback_value,
            },
        }))
    })
}

/// Takes ownership of one leaked reference to the anchor name (0 when absent), one strong
/// reference to the side and one strong reference to the fallback value if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_anchor(
    has_anchor_name: bool,
    anchor_name: usize,
    anchor_side: *const c_void,
    fallback_value: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Anchor {
            has_anchor_name,
            anchor_name: RetainedUtf16FlyString { raw: anchor_name },
            anchor_side: RetainedStyleValue { pointer: anchor_side },
            fallback_value: RetainedStyleValue {
                pointer: fallback_value,
            },
        }))
    })
}

/// Takes ownership of one strong reference to each edge.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_position(
    edge_x: *const c_void,
    edge_y: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Position {
            edge_x: RetainedStyleValue { pointer: edge_x },
            edge_y: RetainedStyleValue { pointer: edge_y },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null style value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_shadow(
    shadow_type: u8,
    color: *const c_void,
    offset_x: *const c_void,
    offset_y: *const c_void,
    blur_radius: *const c_void,
    spread_distance: *const c_void,
    placement: u8,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Shadow {
            shadow_type,
            color: RetainedStyleValue { pointer: color },
            offset_x: RetainedStyleValue { pointer: offset_x },
            offset_y: RetainedStyleValue { pointer: offset_y },
            blur_radius: RetainedStyleValue { pointer: blur_radius },
            spread_distance: RetainedStyleValue {
                pointer: spread_distance,
            },
            placement,
        }))
    })
}

/// Takes ownership of one strong reference to the content list and, when non-null, the
/// alt-text list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_content(
    content: *const c_void,
    alt_text: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Content {
            content: RetainedStyleValue { pointer: content },
            alt_text: RetainedStyleValue { pointer: alt_text },
        }))
    })
}

/// Takes ownership of one leaked reference to each string and one strong reference to the
/// counter style.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter(
    function: u8,
    counter_name: usize,
    counter_style: *const c_void,
    join_string: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Counter {
            function,
            counter_name: RetainedUtf16FlyString { raw: counter_name },
            counter_style: RetainedStyleValue { pointer: counter_style },
            join_string: RetainedUtf16FlyString { raw: join_string },
        }))
    })
}

/// Takes ownership of one strong reference to each color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_light_dark(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    light: *const c_void,
    dark: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::LightDark {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            light: RetainedStyleValue { pointer: light },
            dark: RetainedStyleValue { pointer: dark },
        }))
    })
}

/// Takes ownership of one strong reference to the fixed value and one leaked reference to the
/// name when they are present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_random_value_sharing(
    fixed_value: *const c_void,
    is_auto: bool,
    has_name: bool,
    name: usize,
    element_shared: bool,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::RandomValueSharing {
            fixed_value: RetainedStyleValue { pointer: fixed_value },
            is_auto,
            has_name,
            name: RetainedUtf16FlyString { raw: name },
            element_shared,
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_scrollbar_gutter(value: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::ScrollbarGutter { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_color_interpolation_method(
    is_polar: bool,
    color_space: u8,
    hue_interpolation_method: u8,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ColorInterpolationMethod {
            is_polar,
            color_space,
            hue_interpolation_method,
        }))
    })
}

/// Takes ownership of one strong reference to each of the `length` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_value_list(
    values: *const *const c_void,
    length: usize,
    separator: u8,
    collapsible: bool,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ValueList {
            values: unsafe { RetainedStyleValueList::from_raw(values, length) },
            separator,
            collapsible,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_tuple(
    values: *const *const c_void,
    length: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Tuple {
            values: unsafe { RetainedStyleValueList::from_raw(values, length) },
        }))
    })
}

/// Takes ownership of one strong reference to each value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_transformation(
    property: u16,
    transform_function: u8,
    values: *const *const c_void,
    length: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Transformation {
            property,
            transform_function,
            values: unsafe { RetainedStyleValueList::from_raw(values, length) },
        }))
    })
}

/// Takes ownership of one strong reference to each value; the property ids are copied.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_shorthand(
    shorthand_property: u16,
    sub_properties: *const u16,
    sub_property_count: usize,
    values: *const *const c_void,
    value_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Shorthand {
            shorthand_property,
            sub_properties: unsafe { RetainedPropertyIdList::from_raw(sub_properties, sub_property_count) },
            values: unsafe { RetainedStyleValueList::from_raw(values, value_count) },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_display(raw: u32) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Display { raw })))
}

/// Takes ownership of one strong reference to each non-null component value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_radial_size(
    component_count: u8,
    is_extent_0: bool,
    extent_0: u8,
    value_0: *const c_void,
    is_extent_1: bool,
    extent_1: u8,
    value_1: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::RadialSize {
            component_count,
            is_extent_0,
            extent_0,
            value_0: RetainedStyleValue { pointer: value_0 },
            is_extent_1,
            extent_1,
            value_1: RetainedStyleValue { pointer: value_1 },
        }))
    })
}

/// Takes ownership of one leaked reference to the URL string and to each modifier's retained
/// string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_url(
    url: usize,
    url_type: u8,
    modifiers: *const RetainedRequestUrlModifier,
    modifier_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Url {
            url: RetainedString { raw: url },
            url_type,
            modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(modifiers, modifier_count) },
        }))
    })
}

/// Takes ownership of one strong reference to the local name if local, or one leaked reference
/// to the URL string and each modifier string otherwise, plus the format string when present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_font_source(
    is_local: bool,
    local_name: *const c_void,
    url: usize,
    url_type: u8,
    url_modifiers: *const RetainedRequestUrlModifier,
    url_modifier_count: usize,
    has_format: bool,
    format: usize,
    tech: *const u8,
    tech_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::FontSource {
            is_local,
            local_name: RetainedStyleValue { pointer: local_name },
            url: RetainedString { raw: url },
            url_type,
            url_modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(url_modifiers, url_modifier_count) },
            has_format,
            format: RetainedUtf16FlyString { raw: format },
            tech: unsafe { RetainedByteList::from_raw(tech, tech_count) },
        }))
    })
}

/// Takes ownership of one leaked reference to each scheme name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_color_scheme(
    schemes: *const usize,
    scheme_count: usize,
    only: bool,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ColorScheme {
            schemes: unsafe { RetainedUtf16FlyStringList::from_raw(schemes, scheme_count) },
            only,
        }))
    })
}

/// Takes ownership of one leaked reference to each string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_unresolved(
    source_text: usize,
    value_comparison_text: usize,
    presence_attr: bool,
    presence_dashed_function: bool,
    presence_env: bool,
    presence_if: bool,
    presence_inherit: bool,
    presence_var: bool,
    contains_attr_tainted_values: bool,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Unresolved {
            source_text: RetainedString { raw: source_text },
            value_comparison_text: RetainedString {
                raw: value_comparison_text,
            },
            presence_attr,
            presence_dashed_function,
            presence_env,
            presence_if,
            presence_inherit,
            presence_var,
            contains_attr_tainted_values,
        }))
    })
}

/// Takes ownership of the definitions' retained strings and values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_definitions(
    definitions: *const RetainedCounterDefinition,
    length: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::CounterDefinitions {
            counter_definitions: unsafe { RetainedCounterDefinitionList::from_raw(definitions, length) },
        }))
    })
}

/// Takes ownership of one strong reference to the value and one leaked reference to the name
/// when they are present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_track_placement(
    kind: u8,
    value: *const c_void,
    has_name: bool,
    name: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::GridTrackPlacement {
            kind,
            value: RetainedStyleValue { pointer: value },
            has_name,
            name: RetainedUtf16FlyString { raw: name },
        }))
    })
}

/// Takes ownership of one strong reference to the first symbol and one leaked reference to the
/// name when they are present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_style_system(
    kind: u8,
    system: u8,
    first_symbol: *const c_void,
    name: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::CounterStyleSystem {
            kind,
            system,
            first_symbol: RetainedStyleValue { pointer: first_symbol },
            name: RetainedUtf16FlyString { raw: name },
        }))
    })
}

/// Takes ownership of one leaked reference to the name (0 when this is a symbols() function)
/// and to each symbol string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_style(
    is_symbols: bool,
    name: usize,
    symbols_type: u8,
    symbols: *const usize,
    symbol_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::CounterStyle {
            is_symbols,
            name: RetainedUtf16FlyString { raw: name },
            symbols_type,
            symbols: unsafe { RetainedUtf16FlyStringList::from_raw(symbols, symbol_count) },
        }))
    })
}

/// Takes ownership of one strong reference to the image and to each non-null coordinate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_cursor(
    image: *const c_void,
    x: *const c_void,
    y: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Cursor {
            image: RetainedStyleValue { pointer: image },
            x: RetainedStyleValue { pointer: x },
            y: RetainedStyleValue { pointer: y },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and one leaked reference to
/// the name when present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_color_function(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    channel_0: *const c_void,
    channel_1: *const c_void,
    channel_2: *const c_void,
    alpha: *const c_void,
    has_name: bool,
    name: usize,
    origin_color: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ColorFunction {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            channel_0: RetainedStyleValue { pointer: channel_0 },
            channel_1: RetainedStyleValue { pointer: channel_1 },
            channel_2: RetainedStyleValue { pointer: channel_2 },
            alpha: RetainedStyleValue { pointer: alpha },
            has_name,
            name: RetainedUtf16FlyString { raw: name },
            origin_color: RetainedStyleValue { pointer: origin_color },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_color_mix(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    color_interpolation_method: *const c_void,
    first_color: *const c_void,
    first_percentage: *const c_void,
    second_color: *const c_void,
    second_percentage: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ColorMix {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            color_interpolation_method: RetainedStyleValue {
                pointer: color_interpolation_method,
            },
            first_color: RetainedStyleValue { pointer: first_color },
            first_percentage: RetainedStyleValue {
                pointer: first_percentage,
            },
            second_color: RetainedStyleValue { pointer: second_color },
            second_percentage: RetainedStyleValue {
                pointer: second_percentage,
            },
        }))
    })
}

/// Takes ownership of the options' retained values and strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_image_set(
    options: *const RetainedImageSetOption,
    length: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ImageSet {
            options: unsafe { RetainedImageSetOptionList::from_raw(options, length) },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_linear_gradient(
    has_direction_value: bool,
    direction_value: *const c_void,
    side_or_corner: u8,
    stops: *const RetainedColorStop,
    stop_count: usize,
    gradient_type: u8,
    repeating: bool,
    color_interpolation_method: *const c_void,
    color_syntax: u8,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::LinearGradient {
            has_direction_value,
            direction_value: RetainedStyleValue {
                pointer: direction_value,
            },
            side_or_corner,
            color_stop_list: unsafe { RetainedColorStopList::from_raw(stops, stop_count) },
            gradient_type,
            repeating,
            color_interpolation_method: RetainedStyleValue {
                pointer: color_interpolation_method,
            },
            color_syntax,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_conic_gradient(
    from_angle: *const c_void,
    position: *const c_void,
    stops: *const RetainedColorStop,
    stop_count: usize,
    repeating: bool,
    color_interpolation_method: *const c_void,
    color_syntax: u8,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ConicGradient {
            from_angle: RetainedStyleValue { pointer: from_angle },
            position: RetainedStyleValue { pointer: position },
            color_stop_list: unsafe { RetainedColorStopList::from_raw(stops, stop_count) },
            repeating,
            color_interpolation_method: RetainedStyleValue {
                pointer: color_interpolation_method,
            },
            color_syntax,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_radial_gradient(
    ending_shape: u8,
    size: *const c_void,
    position: *const c_void,
    stops: *const RetainedColorStop,
    stop_count: usize,
    repeating: bool,
    color_interpolation_method: *const c_void,
    color_syntax: u8,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::RadialGradient {
            ending_shape,
            size: RetainedStyleValue { pointer: size },
            position: RetainedStyleValue { pointer: position },
            color_stop_list: unsafe { RetainedColorStopList::from_raw(stops, stop_count) },
            repeating,
            color_interpolation_method: RetainedStyleValue {
                pointer: color_interpolation_method,
            },
            color_syntax,
        }))
    })
}

/// Takes ownership of the areas' retained names.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_template_area(
    areas: *const RetainedGridArea,
    area_count: usize,
    row_count: usize,
    column_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::GridTemplateArea {
            grid_areas: unsafe { RetainedGridAreaList::from_raw(areas, area_count) },
            row_count,
            column_count,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_easing(
    kind: u8,
    linear_stops: *const RetainedLinearEasingStop,
    linear_stop_count: usize,
    x1: *const c_void,
    y1: *const c_void,
    x2: *const c_void,
    y2: *const c_void,
    number_of_intervals: *const c_void,
    step_position: u8,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Easing {
            kind,
            linear_stops: unsafe { RetainedLinearEasingStopList::from_raw(linear_stops, linear_stop_count) },
            x1: RetainedStyleValue { pointer: x1 },
            y1: RetainedStyleValue { pointer: y1 },
            x2: RetainedStyleValue { pointer: x2 },
            y2: RetainedStyleValue { pointer: y2 },
            number_of_intervals: RetainedStyleValue {
                pointer: number_of_intervals,
            },
            step_position,
        }))
    })
}

/// Takes ownership of the entries' retained values and names, recursively.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_track_size_list(
    is_subgrid: bool,
    preserve_line_name_sets: bool,
    entries: *const GridTrackEntryInput,
    entry_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::GridTrackSizeList {
            is_subgrid,
            preserve_line_name_sets,
            entries: unsafe { RetainedGridTrackEntryList::from_raw(entries, entry_count) },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value, of the points' retained
/// values and of one leaked reference to the path string when present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_basic_shape(
    kind: u8,
    v0: *const c_void,
    v1: *const c_void,
    v2: *const c_void,
    v3: *const c_void,
    v4: *const c_void,
    fill_rule: u8,
    points: *const RetainedShapePoint,
    point_count: usize,
    path_string: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::BasicShape {
            kind,
            v0: RetainedStyleValue { pointer: v0 },
            v1: RetainedStyleValue { pointer: v1 },
            v2: RetainedStyleValue { pointer: v2 },
            v3: RetainedStyleValue { pointer: v3 },
            v4: RetainedStyleValue { pointer: v4 },
            fill_rule,
            points: unsafe { RetainedShapePointList::from_raw(points, point_count) },
            path_string: RetainedUtf16FlyString { raw: path_string },
        }))
    })
}

/// Takes ownership of one strong reference to the calculation node; the byte blob and ranges
/// are copied.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_calculated(
    rust_calculation: *const crate::calc::CalcNode,
    calculation: *const c_void,
    resolved_type: *const u8,
    resolved_type_length: usize,
    has_percentages_resolve_as: bool,
    resolve_as_is_number: bool,
    resolve_as_base: u8,
    percentages_resolve_as: u8,
    resolve_numbers_as_integers: bool,
    accepted_ranges: *const RetainedNumericRangeByType,
    accepted_range_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Calculated {
            rust_calculation: unsafe { crate::calc::CalcNodeHandle::from_raw(rust_calculation) },
            resolve_as_is_number,
            resolve_as_base,
            calculation: RetainedCalculationNode { pointer: calculation },
            resolved_type: unsafe { RetainedByteList::from_raw(resolved_type, resolved_type_length) },
            has_percentages_resolve_as,
            percentages_resolve_as,
            resolve_numbers_as_integers,
            accepted_ranges: unsafe { RetainedNumericRangeList::from_raw(accepted_ranges, accepted_range_count) },
        }))
    })
}

/// Takes ownership of one leaked reference to the URL string and each modifier string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_image(
    url: usize,
    url_type: u8,
    url_modifiers: *const RetainedRequestUrlModifier,
    url_modifier_count: usize,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Image {
            url: RetainedString { raw: url },
            url_type,
            url_modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(url_modifiers, url_modifier_count) },
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_destroy(value: *mut StyleValueData) {
    abort_on_panic(|| {
        if value.is_null() {
            return;
        }
        drop(unsafe { Box::from_raw(value) });
    });
}

/// Whether a value's computed color depends on the element's used currentcolor: the
/// currentcolor keyword itself, or a color function, color-mix(), contrast-color() or
/// light-dark() whose nested colors do. `data_of` maps a nested value's shell pointer to
/// its Rust-owned data.
fn value_depends_on_current_color(
    value: &StyleValueData,
    data_of: unsafe extern "C" fn(*const c_void) -> *const c_void,
) -> bool {
    let retained_depends = |retained: &RetainedStyleValue| -> bool {
        let shell = retained.shell_pointer();
        if shell.is_null() {
            return false;
        }
        let data = unsafe { data_of(shell) };
        value_depends_on_current_color(unsafe { &*(data as *const StyleValueData) }, data_of)
    };
    match value {
        StyleValueData::Keyword { keyword } => *keyword == crate::style_compute::keyword::CURRENTCOLOR,
        StyleValueData::ColorFunction { origin_color, .. } => retained_depends(origin_color),
        StyleValueData::ColorMix {
            first_color,
            second_color,
            ..
        } => retained_depends(first_color) || retained_depends(second_color),
        StyleValueData::ContrastColor { color, .. } => retained_depends(color),
        StyleValueData::LightDark { light, dark, .. } => retained_depends(light) || retained_depends(dark),
        _ => false,
    }
}

/// # Safety
/// `data` must point at a valid StyleValueData and `data_of` must be a valid callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_depends_on_current_color(
    data: *const c_void,
    data_of: unsafe extern "C" fn(shell: *const c_void) -> *const c_void,
) -> bool {
    crate::abort_on_panic(|| value_depends_on_current_color(unsafe { &*(data as *const StyleValueData) }, data_of))
}
