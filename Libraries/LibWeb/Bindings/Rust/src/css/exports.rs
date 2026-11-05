/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the C++ code of LibWeb asks of the interfaces of CSS, declared in Libraries/LibWeb/CSS/MediaList.cpp and
//! Libraries/LibWeb/CSS/TypedOM.cpp.

use core::ffi::c_void;
use core::ptr::{NonNull, null_mut};
use std::rc::Rc;
use std::sync::Arc;

use ak::Utf16String;
use libcompositing_rust::css_pixels::{CssPixelRect, FfiCssPixelPoint, FfiCssPixelRect};
use libjs_rust::api::{Gc, MarkedVec, Vm};
use libweb_rust::typed_om::{FfiNumericType, RetainedStyleValue};
use libweb_rust::{NativeMediaList, NativeRule, NativeRuleList};

use super::animation_event::{AnimationEvent, AnimationEventInit};
use super::css_container_rule::CSSContainerRule;
use super::css_counter_style_rule::CSSCounterStyleRule;
use super::css_font_face_rule::CSSFontFaceRule;
use super::css_font_feature_values_rule::CSSFontFeatureValuesRule;
use super::css_function_declarations::CSSFunctionDeclarations;
use super::css_function_rule::CSSFunctionRule;
use super::css_image_value::CSSImageValue;
use super::css_import_rule::CSSImportRule;
use super::css_keyframe_rule::CSSKeyframeRule;
use super::css_keyframes_rule::CSSKeyframesRule;
use super::css_keyword_value::CSSKeywordValue;
use super::css_layer_block_rule::CSSLayerBlockRule;
use super::css_layer_statement_rule::CSSLayerStatementRule;
use super::css_margin_rule::CSSMarginRule;
use super::css_math_clamp::CSSMathClamp;
use super::css_math_invert::CSSMathInvert;
use super::css_math_max::CSSMathMax;
use super::css_math_min::CSSMathMin;
use super::css_math_negate::CSSMathNegate;
use super::css_math_product::CSSMathProduct;
use super::css_math_sum::CSSMathSum;
use super::css_media_rule::CSSMediaRule;
use super::css_namespace_rule::CSSNamespaceRule;
use super::css_nested_declarations::CSSNestedDeclarations;
use super::css_numeric_value::AnyCSSNumericValue;
use super::css_page_rule::CSSPageRule;
use super::css_position_try_rule::CSSPositionTryRule;
use super::css_property_rule::CSSPropertyRule;
use super::css_rule::AnyCSSRule;
use super::css_rule_list::CSSRuleList;
use super::css_scope_rule::CSSScopeRule;
use super::css_style_rule::CSSStyleRule;
use super::css_style_sheet::CSSStyleSheet;
use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue};
use super::css_supports_rule::CSSSupportsRule;
use super::css_transform_value::CSSTransformValue;
use super::css_unit_value::CSSUnitValue;
use super::css_unparsed_value::CSSUnparsedValue;
use super::font_face_set::{CppFontFaceState, FontFaceSet};
use super::highlight_registry::HighlightRegistry;
use super::media_list::MediaList;
use super::media_query_list::MediaQueryList;
use super::screen::Screen;
use super::style_property_map::StylePropertyMap;
use super::style_property_map_read_only::StylePropertyMapReadOnly;
use super::style_sheet_list::StyleSheetList;
use super::transition_event::{TransitionEvent, TransitionEventInit};
use super::typed_om;
use super::visual_viewport::VisualViewport;
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

/// A new MediaQueryList of the document, whose media query list shares the queries of the one that C++ passes.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `document` a live document.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_query_list_create(
    vm: *mut c_void,
    realm: *mut c_void,
    document: *mut c_void,
    media: &NativeMediaList,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM, a live realm and a live document.
    let (vm, realm, document) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (vm, realm, CppCell::from_raw(document))
    };
    cell_into_abi(MediaQueryList::create(vm, realm, document, media.share()))
}

/// # Safety
///
/// `list` must be a live MediaQueryList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_query_list_has_change_listener(list: *mut c_void) -> bool {
    // SAFETY: The caller passes a live MediaQueryList.
    unsafe { cell_from_abi::<MediaQueryList>(list) }.has_change_listener()
}

/// # Safety
///
/// `vm` must be the live VM, and `list` a live MediaQueryList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_media_query_list_evaluate_and_report_change(vm: *mut c_void, list: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live MediaQueryList.
    let (vm, list) = unsafe { (vm_from_abi(vm), cell_from_abi::<MediaQueryList>(list)) };
    list.evaluate_and_report_change(vm);
}

/// A new FontFaceSet of the environment settings object.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `environment` a live environment settings object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_create(
    vm: *mut c_void,
    realm: *mut c_void,
    environment: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM, a live realm and a live environment settings object.
    let (vm, realm, environment) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (vm, realm, CppCell::from_raw(environment))
    };
    cell_into_abi(FontFaceSet::create(vm, realm, environment))
}

/// # Safety
///
/// `set` must be a live FontFaceSet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_size(set: *mut c_void) -> usize {
    // SAFETY: The caller passes a live FontFaceSet.
    unsafe { cell_from_abi::<FontFaceSet>(set) }.size()
}

/// # Safety
///
/// `set` must be a live FontFaceSet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_relevant_settings_object(set: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live FontFaceSet.
    unsafe { cell_from_abi::<FontFaceSet>(set) }
        .relevant_settings_object()
        .as_ptr()
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_add_css_connected_font(
    vm: *mut c_void,
    set: *mut c_void,
    font_face: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, a live FontFaceSet and a live FontFaceState.
    let (vm, set, font_face) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<FontFaceSet>(set),
            CppFontFaceState::from_raw(font_face),
        )
    };
    set.add_css_connected_font(vm, font_face);
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_remove_css_connected_font(
    vm: *mut c_void,
    set: *mut c_void,
    font_face: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, a live FontFaceSet and a live FontFaceState.
    let (vm, set, font_face) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<FontFaceSet>(set),
            CppFontFaceState::from_raw(font_face),
        )
    };
    set.remove_css_connected_font(vm, &font_face);
}

/// # Safety
///
/// `vm` must be the live VM, and `set` a live FontFaceSet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_synchronize_css_connected_font_order(vm: *mut c_void, set: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live FontFaceSet.
    let (vm, set) = unsafe { (vm_from_abi(vm), cell_from_abi::<FontFaceSet>(set)) };
    set.synchronize_css_connected_font_order(vm);
}

/// # Safety
///
/// `vm` must be the live VM, and `set` a live FontFaceSet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_set_is_pending_on_the_environment(
    vm: *mut c_void,
    set: *mut c_void,
    is_pending_on_the_environment: bool,
) {
    // SAFETY: The caller passes the live VM and a live FontFaceSet.
    let (vm, set) = unsafe { (vm_from_abi(vm), cell_from_abi::<FontFaceSet>(set)) };
    set.set_is_pending_on_the_environment(vm, is_pending_on_the_environment);
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_font_started_loading(
    vm: *mut c_void,
    set: *mut c_void,
    font_face: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, a live FontFaceSet and a live FontFaceState.
    let (vm, set, font_face) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<FontFaceSet>(set),
            CppFontFaceState::from_raw(font_face),
        )
    };
    set.font_started_loading(vm, font_face);
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_font_finished_loading(
    vm: *mut c_void,
    set: *mut c_void,
    font_face: *mut c_void,
    succeeded: bool,
) {
    // SAFETY: The caller passes the live VM, a live FontFaceSet and a live FontFaceState.
    let (vm, set, font_face) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<FontFaceSet>(set),
            CppFontFaceState::from_raw(font_face),
        )
    };
    set.font_finished_loading(vm, font_face, succeeded);
}

/// A new HighlightRegistry of the document.
///
/// # Safety
///
/// `vm` must be the live VM, and `document` a live Document.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_highlight_registry_create(vm: *mut c_void, document: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live Document.
    let (vm, document) = unsafe { (vm_from_abi(vm), CppCell::from_raw(document)) };
    cell_into_abi(HighlightRegistry::create(vm, document))
}

/// A new StylePropertyMap of the declaration.
///
/// # Safety
///
/// `vm` must be the live VM, and `declaration` a live CSSStyleDeclaration.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_style_property_map_create(vm: *mut c_void, declaration: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live CSSStyleDeclaration.
    let (vm, declaration) = unsafe { (vm_from_abi(vm), CppCell::from_raw(declaration)) };
    cell_into_abi(StylePropertyMap::create(vm, declaration))
}

/// A new StylePropertyMapReadOnly of the computed style of the element.
///
/// # Safety
///
/// `vm` must be the live VM, and `element` a live Element.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_style_property_map_read_only_create(vm: *mut c_void, element: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live Element.
    let (vm, element) = unsafe { (vm_from_abi(vm), CppCell::from_raw(element)) };
    cell_into_abi(StylePropertyMapReadOnly::create_computed_style(vm, element))
}

/// A new Screen of the window.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `window` the live Window of the realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_screen_create(vm: *mut c_void, realm: *mut c_void, window: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM, a live realm and a live Window.
    let (vm, realm, window) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (vm, realm, CppCell::from_raw(window))
    };
    cell_into_abi(Screen::create(vm, realm, window))
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
    let event = AnimationEvent::create(vm, realm, &event_name, event_init);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
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
    let event = TransitionEvent::create(vm, realm, &event_name, event_init);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
}

fn bubbling_event_init() -> EventInit {
    EventInit {
        bubbles: true,
        cancelable: false,
        composed: false,
    }
}

/// The object of the rule whose cell C++ holds, which C++ names by any of the interfaces it is an object of.
///
/// # Safety
///
/// `cell` must be the live cell of an object of an interface that inherits from CSSRule.
pub(crate) unsafe fn rule_from_abi(cell: *mut c_void) -> AnyCSSRule {
    macro_rules! decode {
        ($($interface:ident),+ $(,)?) => {
            $(
                // SAFETY: The caller passes a live cell.
                if let Some(object) = unsafe { cell_if_abi::<$interface>(cell) } {
                    return AnyCSSRule::$interface(object);
                }
            )+
        };
    }
    decode!(
        CSSStyleRule,
        CSSNestedDeclarations,
        CSSMediaRule,
        CSSSupportsRule,
        CSSContainerRule,
        CSSScopeRule,
        CSSLayerBlockRule,
        CSSLayerStatementRule,
        CSSImportRule,
        CSSFontFaceRule,
        CSSKeyframesRule,
        CSSKeyframeRule,
        CSSPageRule,
        CSSMarginRule,
        CSSNamespaceRule,
        CSSPropertyRule,
        CSSCounterStyleRule,
        CSSFontFeatureValuesRule,
        CSSFunctionRule,
        CSSFunctionDeclarations,
        CSSPositionTryRule,
    );
    unreachable!("nothing creates an object of an interface that only others inherit from")
}

/// The implementation of the CSSRuleList whose cell C++ lends, for as long as C++ keeps the cell alive.
///
/// # Safety
///
/// `list` must be the live cell of a CSSRuleList, and it must outlive `'a`.
unsafe fn rule_list_from_abi<'a>(list: *mut c_void) -> &'a CSSRuleList {
    // SAFETY: The caller passes a live cell of a CSSRuleList.
    let list: Ref<CSSRuleList> = unsafe { cell_from_abi(list) };
    // SAFETY: The caller keeps the cell alive for 'a.
    unsafe { &*list.as_ptr() }
}

/// The cell that C++ passes, which it passes as null for nothing.
///
/// # Safety
///
/// `cell` must be a live cell, or null.
unsafe fn optional_cpp_cell(cell: *mut c_void) -> Option<CppCell> {
    // SAFETY: The caller passes a live cell.
    (!cell.is_null()).then(|| unsafe { CppCell::from_raw(cell) })
}

/// The native rule of the rule, which the rule keeps alive.
///
/// # Safety
///
/// `rule` must be the live cell of a rule.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_native(rule: *mut c_void) -> *const NativeRule {
    // SAFETY: The caller passes a live cell.
    Rc::as_ptr(unsafe { rule_from_abi(rule) }.native_rule())
}

/// # Safety
///
/// `rule` must be the live cell of a rule.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_parent_rule(rule: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell.
    let rule = unsafe { rule_from_abi(rule) };
    rule.parent_rule_object().map_or(null_mut(), AnyCSSRule::cell)
}

/// The cell of the CSSStyleSheet that the rule is in.
///
/// # Safety
///
/// `rule` must be the live cell of a rule.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_parent_style_sheet(rule: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell.
    let rule = unsafe { rule_from_abi(rule) };
    rule.parent_sheet().map_or(null_mut(), |sheet| sheet.as_ptr())
}

/// # Safety
///
/// `vm` must be the live VM, and `rule` the live cell of a rule.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_serialized(vm: *mut c_void, rule: *mut c_void) -> usize {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, rule) = unsafe { (vm_from_abi(vm), rule_from_abi(rule)) };
    rule.serialized(vm).into_raw()
}

/// The cell of the CSSStyleDeclaration of the declarations of the rule.
///
/// # Safety
///
/// `vm` must be the live VM, and `rule` the live cell of a rule with declarations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_declarations(vm: *mut c_void, rule: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, rule) = unsafe { (vm_from_abi(vm), rule_from_abi(rule)) };
    rule.declarations(vm).as_ptr()
}

/// The cell of the list of child rules of the rule.
///
/// # Safety
///
/// `vm` must be the live VM, and `rule` the live cell of a rule with child rules.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_child_rules(vm: *mut c_void, rule: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, rule) = unsafe { (vm_from_abi(vm), rule_from_abi(rule)) };
    cell_into_abi(rule.child_rules(vm).expect("C++ asks a rule with child rules"))
}

/// A new CSSRuleList of the native rules, whose @import rules `document` loads, if it is not null.
///
/// # Safety
///
/// `vm` must be the live VM, `rules` a live list, and `document` the live cell of a Document, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_create(
    vm: *mut c_void,
    rules: *const NativeRuleList,
    document: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM, a live list and a live cell, or null.
    let (vm, rules, document) = unsafe {
        (
            vm_from_abi(vm),
            NativeRuleList::retained_from_raw(rules),
            optional_cpp_cell(document),
        )
    };
    cell_into_abi(allocate(vm, |defer_gc| CSSRuleList::new(defer_gc, rules, document)))
}

/// The native rules of the list, which the list keeps alive.
///
/// # Safety
///
/// `list` must be the live cell of a CSSRuleList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_native_rules(list: *mut c_void) -> *const NativeRuleList {
    // SAFETY: The caller passes a live cell, and keeps it alive for the call.
    Rc::as_ptr(unsafe { rule_list_from_abi(list) }.native_rules())
}

/// # Safety
///
/// `list` must be the live cell of a CSSRuleList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_length(list: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell, and keeps it alive for the call.
    unsafe { rule_list_from_abi(list) }.native_length()
}

/// The cell of the rule at the index, which has to be in the list.
///
/// # Safety
///
/// `vm` must be the live VM, and `list` the live cell of a CSSRuleList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_item(vm: *mut c_void, list: *mut c_void, index: usize) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live cell, and keeps it alive for the call.
    let (vm, list) = unsafe { (vm_from_abi(vm), rule_list_from_abi(list)) };
    list.rule_at(vm, index).cell()
}

/// The cell of the object of the rule with the identity, if the rule has one yet.
///
/// # Safety
///
/// `list` must be the live cell of a CSSRuleList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_existing_rule(list: *mut c_void, identity: u64) -> *mut c_void {
    // SAFETY: The caller passes a live cell, and keeps it alive for the call.
    let list = unsafe { rule_list_from_abi(list) };
    list.existing_object(identity).map_or(null_mut(), AnyCSSRule::cell)
}

/// # Safety
///
/// `vm` must be the live VM, `list` the live cell of a CSSRuleList, and `sheet` that of a CSSStyleSheet, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_set_parent_style_sheet(
    vm: *mut c_void,
    list: *mut c_void,
    sheet: *mut c_void,
) {
    // SAFETY: The caller passes the live VM and live cells, and keeps them alive for the call.
    let (vm, list, sheet) = unsafe { (vm_from_abi(vm), rule_list_from_abi(list), optional_cpp_cell(sheet)) };
    list.set_parent_style_sheet(vm, sheet);
}

/// # Safety
///
/// `vm` must be the live VM, `list` the live cell of a CSSRuleList, `rules` a live list, and `document` the live cell
/// of a Document, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_set_rules(
    vm: *mut c_void,
    list: *mut c_void,
    rules: &NativeRuleList,
    document: *mut c_void,
) {
    // SAFETY: The caller passes the live VM and live cells, and keeps them alive for the call.
    let (vm, list, document) = unsafe { (vm_from_abi(vm), rule_list_from_abi(list), optional_cpp_cell(document)) };
    list.set_rules(vm, rules, document);
}

/// # Safety
///
/// `vm` must be the live VM, and `list` the live cell of a CSSRuleList.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_remove_rule(vm: *mut c_void, list: *mut c_void, index: usize) {
    // SAFETY: The caller passes the live VM and a live cell, and keeps it alive for the call.
    let (vm, list) = unsafe { (vm_from_abi(vm), rule_list_from_abi(list)) };
    list.remove_a_css_rule_without_validation(vm, index);
}

/// A new CSSImportRule for the import that `import` holds, in the sheet `sheet`, if it is not null.
///
/// # Safety
///
/// `vm` must be the live VM, `rule` a live rule, `import` the live cell of a StyleSheetImportCell, and `sheet` that of
/// a CSSStyleSheet, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_import_rule_create(
    vm: *mut c_void,
    rule: *const NativeRule,
    import: *mut c_void,
    sheet: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM, a live rule and live cells, or null for the sheet.
    let (vm, rule, import, sheet) = unsafe {
        (
            vm_from_abi(vm),
            NativeRule::retained_from_raw(rule),
            CppCell::from_raw(import),
            optional_cpp_cell(sheet),
        )
    };
    let object = CSSImportRule::finish(CSSImportRule::create_for_import(vm, rule, import), vm);
    object.store_parent_style_sheet(vm, sheet);
    object.cell()
}

/// The cell of the StyleSheetImportCell of the rule.
///
/// # Safety
///
/// `rule` must be the live cell of a CSSImportRule.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_import_rule_import(rule: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell of a CSSImportRule.
    let rule: Ref<CSSImportRule> = unsafe { cell_from_abi(rule) };
    rule.import_cell()
}

/// # Safety
///
/// `rule` must be the live cell of a CSSStyleRule, and `text` a string that the caller hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_rule_set_selector_text(rule: *mut c_void, text: usize) {
    // SAFETY: The caller passes a live cell of a CSSStyleRule, and hands over the string.
    let (rule, text): (Ref<CSSStyleRule>, _) = unsafe { (cell_from_abi(rule), Utf16String::from_raw_owned(text)) };
    rule.replace_selectors(&text);
}

/// A new CSSStyleSheet for the cell that holds the state of the sheet.
///
/// # Safety
///
/// `vm` must be the live VM, and `state` a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_sheet_create(vm: *mut c_void, state: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, state) = unsafe { (vm_from_abi(vm), CppCell::from_raw(state)) };
    cell_into_abi(CSSStyleSheet::create(vm, state))
}

/// The cell that holds the state of the sheet.
///
/// # Safety
///
/// `sheet` must be the live cell of a CSSStyleSheet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_sheet_state(sheet: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell of a CSSStyleSheet.
    let sheet: Ref<CSSStyleSheet> = unsafe { cell_from_abi(sheet) };
    sheet.state_cell().as_ptr()
}

/// A new StyleSheetList of the sheets of `scope`, which belongs to `node`.
///
/// # Safety
///
/// `vm` must be the live VM, and `node` the live cell of the Document or ShadowRoot that owns `scope`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_sheet_list_create(
    vm: *mut c_void,
    scope: *mut c_void,
    node: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, node) = unsafe { (vm_from_abi(vm), CppCell::from_raw(node)) };
    cell_into_abi(allocate(vm, |_| StyleSheetList::new(scope, node)))
}

/// A new VisualViewport of the document.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `document` a live document.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_create(
    vm: *mut c_void,
    realm: *mut c_void,
    document: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM, a live realm and a live document.
    let (vm, realm, document) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (vm, realm, CppCell::from_raw(document))
    };
    cell_into_abi(VisualViewport::create(vm, realm, document))
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_event_target(viewport: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }
        .event_target()
        .as_ptr()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_offset(viewport: *mut c_void) -> FfiCssPixelPoint {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.offset().into()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_offset_left(viewport: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.offset_left_value()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_offset_top(viewport: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.offset_top_value()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_page_left(viewport: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.page_left_value()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_page_top(viewport: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.page_top_value()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_width(viewport: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.width_value()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_height(viewport: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.height_value()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_scale(viewport: *mut c_void) -> f64 {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.scale_value()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_scroll_by(viewport: *mut c_void, delta: FfiCssPixelPoint) {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.scroll_by(delta.into());
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_zoom(viewport: *mut c_void, position: FfiCssPixelPoint, scale_delta: f64) {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.zoom(position.into(), scale_delta);
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_set_scale_zoomed_by_compositor(viewport: *mut c_void, scale: f64) {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.set_scale_zoomed_by_compositor(scale);
}

/// Whether the scale changed since the compositor last heard of it, in which case `scale` receives it.
///
/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_take_scale_for_compositor(viewport: *mut c_void, scale: &mut f64) -> bool {
    // SAFETY: The caller passes a live VisualViewport.
    let taken = unsafe { cell_from_abi::<VisualViewport>(viewport) }.take_scale_for_compositor();
    if let Some(taken) = taken {
        *scale = taken;
    }
    taken.is_some()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_map_to_layout_viewport(
    viewport: *mut c_void,
    position: FfiCssPixelPoint,
) -> FfiCssPixelPoint {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }
        .map_to_layout_viewport(position.into())
        .into()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_map_point_from_layout_viewport(
    viewport: *mut c_void,
    position: FfiCssPixelPoint,
) -> FfiCssPixelPoint {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }
        .map_point_from_layout_viewport(position.into())
        .into()
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_map_rect_from_layout_viewport(
    viewport: *mut c_void,
    rect: FfiCssPixelRect,
) -> FfiCssPixelRect {
    // SAFETY: The caller passes a live VisualViewport.
    let viewport = unsafe { cell_from_abi::<VisualViewport>(viewport) };
    let mapped = viewport.map_rect_from_layout_viewport(CssPixelRect::new(rect.x, rect.y, rect.width, rect.height));
    FfiCssPixelRect {
        x: mapped.x,
        y: mapped.y,
        width: mapped.width,
        height: mapped.height,
    }
}

/// # Safety
///
/// `viewport` must be a live VisualViewport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_visual_viewport_reset(viewport: *mut c_void) {
    // SAFETY: The caller passes a live VisualViewport.
    unsafe { cell_from_abi::<VisualViewport>(viewport) }.reset();
}
