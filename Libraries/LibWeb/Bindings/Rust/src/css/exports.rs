/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the C++ code of LibWeb asks of the interfaces of CSS, declared in Libraries/LibWeb/CSS/MediaList.cpp and
//! Libraries/LibWeb/CSS/TypedOM.cpp.

use core::ffi::c_void;
use core::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{Gc, MarkedVec, Vm};
use libweb_rust::NativeMediaList;
use libweb_rust::typed_om::{FfiNumericType, RetainedStyleValue};

use super::animation_event::{AnimationEvent, AnimationEventInit};
use super::css_image_value::CSSImageValue;
use super::css_keyword_value::CSSKeywordValue;
use super::css_math_clamp::CSSMathClamp;
use super::css_math_invert::CSSMathInvert;
use super::css_math_max::CSSMathMax;
use super::css_math_min::CSSMathMin;
use super::css_math_negate::CSSMathNegate;
use super::css_math_product::CSSMathProduct;
use super::css_math_sum::CSSMathSum;
use super::css_numeric_value::AnyCSSNumericValue;
use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue, PerformTypeCheck};
use super::css_transform_value::CSSTransformValue;
use super::css_unit_value::CSSUnitValue;
use super::css_unparsed_value::CSSUnparsedValue;
use super::media_list::MediaList;
use super::media_query_list_event::{MediaQueryListEvent, MediaQueryListEventInit};
use super::transition_event::{TransitionEvent, TransitionEventInit};
use super::typed_om;
use crate::bindings::Exception;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, allocate, cell_from_abi, cell_if_abi, cell_into_abi, realm_arguments};
use crate::dom::event::EventInit;

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

/// The object of the cell of a CSSStyleValue or of an interface that inherits from it.
///
/// # Safety
///
/// `cell` must be the live cell of such an object.
unsafe fn style_value_from_abi(cell: *mut c_void) -> AnyCSSStyleValue {
    macro_rules! decode {
        ($($interface:ident),+ $(,)?) => {
            $(
                // SAFETY: The caller passes a live cell.
                if let Some(object) = unsafe { cell_if_abi::<$interface>(cell) } {
                    return AnyCSSStyleValue::$interface(object);
                }
            )+
        };
    }
    decode!(
        CSSUnitValue,
        CSSMathSum,
        CSSMathProduct,
        CSSMathNegate,
        CSSMathInvert,
        CSSMathMin,
        CSSMathMax,
        CSSMathClamp,
        CSSKeywordValue,
        CSSUnparsedValue,
        CSSTransformValue,
        CSSImageValue,
        CSSStyleValue,
    );
    unreachable!("C++ passes the cell of a CSSStyleValue")
}

/// The object of the cell of a CSSNumericValue.
///
/// # Safety
///
/// `cell` must be the live cell of a CSSNumericValue.
unsafe fn numeric_value_from_abi(cell: *mut c_void) -> AnyCSSNumericValue {
    // SAFETY: The caller passes a live cell.
    unsafe { style_value_from_abi(cell) }
        .as_numeric()
        .expect("C++ passes the cell of a CSSNumericValue")
}

/// Hands the message of a type error to C++, which throws it, as the steps that C++ calls throw nothing else.
fn type_error_into_abi(exception: Exception, error: *mut usize) {
    let Exception::TypeError(message) = exception else {
        unreachable!("the steps of the CSS Typed OM that C++ calls only throw TypeErrors");
    };
    // SAFETY: The caller of the export passes a place for the string.
    unsafe { *error = message.into_raw() };
}

/// A new CSSStyleValue, given a raw string and a style value that C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, `associated_property` a raw string and `source_value` a strong reference to a style value
/// that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_value_create(
    vm: *mut c_void,
    associated_property: usize,
    source_value: *const c_void,
    serialization_mode: u8,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller hands over the string and the style value.
    let (associated_property, source_value) = unsafe {
        (
            Utf16String::from_raw_owned(associated_property),
            RetainedStyleValue::from_raw(source_value),
        )
    };
    cell_into_abi(CSSStyleValue::create(
        vm,
        associated_property,
        source_value,
        serialization_mode,
    ))
}

/// # Safety
///
/// `vm` must be the live VM, `value` a strong reference to a style value and `associated_property` a raw string that
/// C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_value_reify(
    vm: *mut c_void,
    value: *const c_void,
    associated_property: usize,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller hands over the string and the style value.
    let (value, associated_property) = unsafe {
        (
            RetainedStyleValue::from_raw(value),
            Utf16String::from_raw_owned(associated_property),
        )
    };
    typed_om::reify(vm, &value, &associated_property).cell_into_abi()
}

/// The first value of parsing a CSSStyleValue, or null with the message of the TypeError in `error`.
///
/// # Safety
///
/// `vm` must be the live VM, `property` and `css_text` raw strings that C++ hands over, and `error` a place for a raw
/// string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_value_parse(
    vm: *mut c_void,
    property: usize,
    css_text: usize,
    error: *mut usize,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller hands over the strings.
    let (property, css_text) = unsafe {
        (
            Utf16String::from_raw_owned(property),
            Utf16String::from_raw_owned(css_text),
        )
    };
    match typed_om::parse_values(vm, property, css_text) {
        Ok(values) => values
            .get(0)
            .expect("parsing results in at least one value")
            .cell_into_abi(),
        Err(exception) => {
            type_error_into_abi(exception, error);
            core::ptr::null_mut()
        }
    }
}

/// Whether the value has an associated property, which goes to `associated_property` as a raw string.
///
/// # Safety
///
/// `value` must be the live cell of a CSSStyleValue, and `associated_property` a place for a raw string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_value_associated_property(
    value: *mut c_void,
    associated_property: *mut usize,
) -> bool {
    // SAFETY: The caller passes a live cell.
    let value = unsafe { style_value_from_abi(value) };
    let Some(property) = value.style_value().associated_property() else {
        return false;
    };
    // SAFETY: The caller passes a place for the string.
    unsafe { *associated_property = property.clone().into_raw() };
    true
}

/// The internal representation as a strong reference, or null with the message of the TypeError in `error`.
///
/// # Safety
///
/// `vm` must be the live VM, `value` the live cell of a CSSStyleValue, `property_name` a raw string that C++ hands over,
/// and `error` a place for a raw string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_value_create_an_internal_representation(
    vm: *mut c_void,
    value: *mut c_void,
    property_id: u16,
    property_name: usize,
    perform_type_check: bool,
    error: *mut usize,
) -> *const c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live cell.
    let value = unsafe { style_value_from_abi(value) };
    // SAFETY: The caller hands over the string.
    let property_name = unsafe { Utf16String::from_raw_owned(property_name) };
    let perform_type_check = if perform_type_check {
        PerformTypeCheck::Yes
    } else {
        PerformTypeCheck::No
    };
    match value.create_an_internal_representation(vm, property_id, &property_name, perform_type_check) {
        Ok(representation) => representation.into_raw(),
        Err(exception) => {
            type_error_into_abi(exception, error);
            core::ptr::null()
        }
    }
}

/// What C++ distinguishes CSSStyleValues by, whose order is that of `enum class StyleValueKind` in
/// Libraries/LibWeb/CSS/TypedOM.cpp.
#[repr(u8)]
enum StyleValueKind {
    Other,
    Numeric,
    Unit,
    Unparsed,
}

/// # Safety
///
/// `value` must be the live cell of a CSSStyleValue.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_value_kind(value: *mut c_void) -> u8 {
    // SAFETY: The caller passes a live cell.
    let value = unsafe { style_value_from_abi(value) };
    let kind = match value {
        AnyCSSStyleValue::CSSUnitValue(_) => StyleValueKind::Unit,
        AnyCSSStyleValue::CSSUnparsedValue(_) => StyleValueKind::Unparsed,
        value if value.as_numeric().is_some() => StyleValueKind::Numeric,
        _ => StyleValueKind::Other,
    };
    kind as u8
}

/// # Safety
///
/// `vm` must be the live VM, and `first` and `second` live cells of CSSNumericValues whose types can be added.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_numeric_value_create_sum(
    vm: *mut c_void,
    first: *mut c_void,
    second: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    let values = MarkedVec::new(vm);
    // SAFETY: The caller passes live cells.
    unsafe {
        values.push(numeric_value_from_abi(first));
        values.push(numeric_value_from_abi(second));
    }
    let Ok(sum) = CSSMathSum::add_all_types_into_math_sum(vm, &values) else {
        unreachable!("C++ passes values whose types can be added");
    };
    cell_into_abi(sum)
}

/// # Safety
///
/// `value` must be the live cell of a CSSNumericValue.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_numeric_value_type(value: *mut c_void) -> FfiNumericType {
    // SAFETY: The caller passes a live cell.
    FfiNumericType::from_calc(Some(unsafe { numeric_value_from_abi(value) }.numeric_type()))
}

/// The serialization of the value, as a raw string for C++.
///
/// # Safety
///
/// `value` must be the live cell of a CSSNumericValue.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_numeric_value_to_string(value: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell.
    unsafe { numeric_value_from_abi(value) }.to_css_string().into_raw()
}

/// A strong reference to the calculation of the value, or null if it has none.
///
/// # Safety
///
/// `value` must be the live cell of a CSSNumericValue.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_numeric_value_create_calculation_node(value: *mut c_void) -> *const c_void {
    // SAFETY: The caller passes a live cell.
    match unsafe { numeric_value_from_abi(value) }.create_calculation_node() {
        Ok(node) => Arc::into_raw(node).cast(),
        Err(_) => core::ptr::null(),
    }
}

/// # Safety
///
/// `vm` must be the live VM, and `unit` a raw string that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_unit_value_create(vm: *mut c_void, value: f64, unit: usize) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller hands over the string.
    let unit = unsafe { Utf16String::from_raw_owned(unit) };
    cell_into_abi(CSSUnitValue::create(vm, value, &typed_om::to_rust_string(&unit)))
}

/// # Safety
///
/// `value` must be the live cell of a CSSUnitValue.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_unit_value_value(value: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live cell of a CSSUnitValue.
    let value: Ref<CSSUnitValue> = unsafe { cell_from_abi(value) };
    value.raw_value()
}

/// # Safety
///
/// `value` must be the live cell of a CSSUnitValue.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_unit_value_unit(value: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell of a CSSUnitValue.
    let value: Ref<CSSUnitValue> = unsafe { cell_from_abi(value) };
    Utf16String::from_utf8(value.raw_unit()).into_raw()
}

/// # Safety
///
/// `value` must be the live cell of a CSSKeywordValue.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_keyword_value_value(value: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell of a CSSKeywordValue.
    let value: Ref<CSSKeywordValue> = unsafe { cell_from_abi(value) };
    value.raw_value().into_raw()
}

/// A new MediaQueryListEvent named `change`, as the Event that C++ dispatches, given a raw string that C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `media` a raw string that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_query_list_event_create(
    vm: *mut c_void,
    realm: *mut c_void,
    media: usize,
    matches: bool,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller hands over the string.
    let media = unsafe { Utf16String::from_raw_owned(media) };
    let event_init = MediaQueryListEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        matches,
        media,
    };
    MediaQueryListEvent::create(vm, realm, &Utf16String::from_ascii(b"change"), event_init)
        .event()
        .as_ptr()
}

/// The object of a wrapper that C++ passes, if it passes one.
///
/// # Safety
///
/// `wrapper` must be null or a live wrapper of an object of an interface that LibWeb implements in C++.
unsafe fn cpp_interface_from_abi(wrapper: *mut c_void) -> Option<CppInterface> {
    let wrapper = NonNull::new(wrapper.cast())?;
    // SAFETY: The caller passes a live wrapper.
    Some(unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) })
}

/// A new AnimationEvent that bubbles, as the Event that C++ dispatches.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, `event_name`, `animation_name` and `pseudo_element` raw strings that
/// C++ hands over, and `animation` a live wrapper of a CSSAnimation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_animation_event_create(
    vm: *mut c_void,
    realm: *mut c_void,
    event_name: usize,
    animation_name: usize,
    elapsed_time: f64,
    pseudo_element: usize,
    animation: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller hands over the strings and passes a live wrapper.
    let (event_name, animation_name, pseudo_element, animation) = unsafe {
        (
            Utf16String::from_raw_owned(event_name),
            Utf16String::from_raw_owned(animation_name),
            Utf16String::from_raw_owned(pseudo_element),
            cpp_interface_from_abi(animation),
        )
    };
    let event_init = AnimationEventInit {
        parent: bubbling_event_init(),
        animation,
        animation_name,
        elapsed_time,
        pseudo_element,
    };
    AnimationEvent::create(vm, realm, &event_name, event_init)
        .event()
        .as_ptr()
}

/// A new TransitionEvent that bubbles, as the Event that C++ dispatches.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, `event_name`, `property_name` and `pseudo_element` raw strings that
/// C++ hands over, and `animation` a live wrapper of a CSSTransition.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_transition_event_create(
    vm: *mut c_void,
    realm: *mut c_void,
    event_name: usize,
    property_name: usize,
    elapsed_time: f64,
    pseudo_element: usize,
    animation: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller hands over the strings and passes a live wrapper.
    let (event_name, property_name, pseudo_element, animation) = unsafe {
        (
            Utf16String::from_raw_owned(event_name),
            Utf16String::from_raw_owned(property_name),
            Utf16String::from_raw_owned(pseudo_element),
            cpp_interface_from_abi(animation),
        )
    };
    let event_init = TransitionEventInit {
        parent: bubbling_event_init(),
        animation,
        elapsed_time,
        property_name,
        pseudo_element,
    };
    TransitionEvent::create(vm, realm, &event_name, event_init)
        .event()
        .as_ptr()
}

fn bubbling_event_init() -> EventInit {
    EventInit {
        bubbles: true,
        cancelable: false,
        composed: false,
    }
}
