/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the C++ code of LibWeb asks of the interfaces of CSS, declared in Libraries/LibWeb/CSS/MediaList.cpp.

use core::ffi::c_void;
use core::ptr::null_mut;
use std::rc::Rc;

use ak::Utf16String;
use libcompositing_rust::css_pixels::{CssPixelRect, FfiCssPixelPoint, FfiCssPixelRect};
use libjs_rust::api::{Gc, Realm, Vm};
use libweb_rust::{NativeMediaList, NativeRule, NativeRuleList};

use super::css_container_rule::CSSContainerRule;
use super::css_counter_style_rule::CSSCounterStyleRule;
use super::css_font_face_rule::CSSFontFaceRule;
use super::css_font_feature_values_rule::CSSFontFeatureValuesRule;
use super::css_function_declarations::CSSFunctionDeclarations;
use super::css_function_rule::CSSFunctionRule;
use super::css_import_rule::CSSImportRule;
use super::css_keyframe_rule::CSSKeyframeRule;
use super::css_keyframes_rule::CSSKeyframesRule;
use super::css_layer_block_rule::CSSLayerBlockRule;
use super::css_layer_statement_rule::CSSLayerStatementRule;
use super::css_margin_rule::CSSMarginRule;
use super::css_media_rule::CSSMediaRule;
use super::css_namespace_rule::CSSNamespaceRule;
use super::css_nested_declarations::CSSNestedDeclarations;
use super::css_page_rule::CSSPageRule;
use super::css_position_try_rule::CSSPositionTryRule;
use super::css_property_rule::CSSPropertyRule;
use super::css_rule::AnyCSSRule;
use super::css_rule_list::CSSRuleList;
use super::css_scope_rule::CSSScopeRule;
use super::css_style_rule::CSSStyleRule;
use super::css_style_sheet::CSSStyleSheet;
use super::css_supports_rule::CSSSupportsRule;
use super::font_face_set::{CppFontFaceState, FontFaceSet};
use super::highlight_registry::HighlightRegistry;
use super::media_list::MediaList;
use super::media_query_list::MediaQueryList;
use super::screen::Screen;
use super::style_property_map::StylePropertyMap;
use super::style_property_map_read_only::StylePropertyMapReadOnly;
use super::style_sheet_list::StyleSheetList;
use super::visual_viewport::VisualViewport;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::interface::{InterfaceCell, Ref, allocate, cell_into_abi};

/// A new MediaList that shares the media queries of `media`.
#[unsafe(no_mangle)]
pub extern "C" fn web_media_list_create(vm: &Vm, media: &NativeMediaList) -> *mut c_void {
    cell_into_abi(allocate(vm, |defer_gc| MediaList::new(defer_gc, media.clone())))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_media_list_set_associated_style_sheet(
    vm: &Vm,
    list: &InterfaceCell,
    sheet: &LentCppCell<cpp_class::CSSStyleSheet>,
) {
    let (list, sheet) = (list.implementation::<MediaList>(), CppCell::from_lent(sheet));
    list.set_associated_style_sheet(vm, sheet);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_media_list_set_associated_rule(
    vm: &Vm,
    list: &InterfaceCell,
    rule: &LentCppCell<cpp_class::CSSRule>,
) {
    let (list, rule) = (list.implementation::<MediaList>(), CppCell::from_lent(rule));
    list.set_associated_rule(vm, rule);
}

/// A native list of its own with the media queries of the MediaList as they are now, which the caller frees.
#[unsafe(no_mangle)]
pub extern "C" fn web_media_list_share_native_list(list: &InterfaceCell) -> *mut NativeMediaList {
    let media = list.implementation::<MediaList>().share_media();
    Box::into_raw(Box::new(media))
}

/// A new MediaQueryList of the document, whose media query list shares the queries of the one that C++ passes.
#[unsafe(no_mangle)]
pub extern "C" fn web_media_query_list_create(
    vm: &Vm,
    realm: Gc<Realm>,
    document: &LentCppCell<cpp_class::Document>,
    media: &NativeMediaList,
) -> *mut c_void {
    let document = CppCell::from_lent(document);
    cell_into_abi(MediaQueryList::create(vm, realm, document, media.share()))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_media_query_list_has_change_listener(list: &InterfaceCell) -> bool {
    list.object::<MediaQueryList>().has_change_listener()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_media_query_list_evaluate_and_report_change(vm: &Vm, list: &InterfaceCell) {
    let list = list.object::<MediaQueryList>();
    list.evaluate_and_report_change(vm);
}

/// A new FontFaceSet of the environment settings object.
#[unsafe(no_mangle)]
pub extern "C" fn web_font_face_set_create(
    vm: &Vm,
    realm: Gc<Realm>,
    environment: &LentCppCell<cpp_class::EnvironmentSettingsObject>,
) -> *mut c_void {
    let environment = CppCell::from_lent(environment);
    cell_into_abi(FontFaceSet::create(vm, realm, environment))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_font_face_set_size(set: &InterfaceCell) -> usize {
    set.object::<FontFaceSet>().size()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_font_face_set_relevant_settings_object(set: &InterfaceCell) -> *mut c_void {
    set.object::<FontFaceSet>().relevant_settings_object().as_ptr()
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_add_css_connected_font(
    vm: &Vm,
    set: &InterfaceCell,
    font_face: *mut c_void,
) {
    // SAFETY: The caller passes a live FontFaceState.
    let (set, font_face) = unsafe { (set.object::<FontFaceSet>(), CppFontFaceState::from_raw(font_face)) };
    set.add_css_connected_font(vm, font_face);
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_remove_css_connected_font(
    vm: &Vm,
    set: &InterfaceCell,
    font_face: *mut c_void,
) {
    // SAFETY: The caller passes a live FontFaceState.
    let (set, font_face) = unsafe { (set.object::<FontFaceSet>(), CppFontFaceState::from_raw(font_face)) };
    set.remove_css_connected_font(vm, &font_face);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_font_face_set_synchronize_css_connected_font_order(vm: &Vm, set: &InterfaceCell) {
    let set = set.object::<FontFaceSet>();
    set.synchronize_css_connected_font_order(vm);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_font_face_set_set_is_pending_on_the_environment(
    vm: &Vm,
    set: &InterfaceCell,
    is_pending_on_the_environment: bool,
) {
    let set = set.object::<FontFaceSet>();
    set.set_is_pending_on_the_environment(vm, is_pending_on_the_environment);
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_font_started_loading(vm: &Vm, set: &InterfaceCell, font_face: *mut c_void) {
    // SAFETY: The caller passes a live FontFaceState.
    let (set, font_face) = unsafe { (set.object::<FontFaceSet>(), CppFontFaceState::from_raw(font_face)) };
    set.font_started_loading(vm, font_face);
}

/// # Safety
///
/// `vm` must be the live VM, `set` a live FontFaceSet, and `font_face` a live FontFaceState.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_font_face_set_font_finished_loading(
    vm: &Vm,
    set: &InterfaceCell,
    font_face: *mut c_void,
    succeeded: bool,
) {
    // SAFETY: The caller passes a live FontFaceState.
    let (set, font_face) = unsafe { (set.object::<FontFaceSet>(), CppFontFaceState::from_raw(font_face)) };
    set.font_finished_loading(vm, font_face, succeeded);
}

/// A new HighlightRegistry of the document.
#[unsafe(no_mangle)]
pub extern "C" fn web_highlight_registry_create(vm: &Vm, document: &LentCppCell<cpp_class::Document>) -> *mut c_void {
    let document = CppCell::from_lent(document);
    cell_into_abi(HighlightRegistry::create(vm, document))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_highlight_registry_has_highlights(registry: &InterfaceCell) -> bool {
    registry.object::<HighlightRegistry>().has_highlights()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_highlight_registry_has_ranges(registry: &InterfaceCell) -> bool {
    registry.object::<HighlightRegistry>().has_ranges()
}

/// # Safety
///
/// `registry` must be a live HighlightRegistry, and `range` a live AbstractRange.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_highlight_registry_contains_range(registry: &InterfaceCell, range: *mut c_void) -> bool {
    registry.object::<HighlightRegistry>().contains_range(range)
}

/// Calls back with the name and the ranges of each registered custom highlight, from the bottom of the stacking order
/// to the top. The callback adopts the name, and the ranges stay alive until it returns.
///
/// # Safety
///
/// `vm` must be the live VM, `registry` a live HighlightRegistry, and `callback` a function that takes `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_highlight_registry_for_each_highlight_in_stacking_order(
    vm: &Vm,
    registry: &InterfaceCell,
    context: *mut c_void,
    callback: unsafe extern "C" fn(context: *mut c_void, name: usize, ranges: *const *mut c_void, range_count: usize),
) {
    let registry = registry.object::<HighlightRegistry>();
    for entry in registry.highlights_in_stacking_order(vm).values().iter() {
        let ranges = entry.highlight.ranges(vm);
        let range_cells: Vec<*mut c_void> = ranges.values().iter().map(CppCell::as_ptr).collect();
        // SAFETY: The caller passes a callback that takes the context, and the marked list keeps the ranges alive.
        unsafe {
            callback(
                context,
                entry.name.clone().into_raw(),
                range_cells.as_ptr(),
                range_cells.len(),
            );
        }
    }
}

/// A new StylePropertyMap of the declaration.
#[unsafe(no_mangle)]
pub extern "C" fn web_style_property_map_create(
    vm: &Vm,
    declaration: &LentCppCell<cpp_class::CSSStyleDeclaration>,
) -> *mut c_void {
    let declaration = CppCell::from_lent(declaration);
    cell_into_abi(StylePropertyMap::create(vm, declaration))
}

/// A new StylePropertyMapReadOnly of the computed style of the element.
#[unsafe(no_mangle)]
pub extern "C" fn web_style_property_map_read_only_create(
    vm: &Vm,
    element: &LentCppCell<cpp_class::Element>,
) -> *mut c_void {
    let element = CppCell::from_lent(element);
    cell_into_abi(StylePropertyMapReadOnly::create_computed_style(vm, element))
}

/// A new Screen of the window.
#[unsafe(no_mangle)]
pub extern "C" fn web_screen_create(vm: &Vm, realm: Gc<Realm>, window: &LentCppCell<cpp_class::Window>) -> *mut c_void {
    let window = CppCell::from_lent(window);
    cell_into_abi(Screen::create(vm, realm, window))
}

/// The object of the rule whose cell C++ holds, which C++ names by any of the interfaces it is an object of.
pub(crate) fn rule_from_abi(cell: &InterfaceCell) -> AnyCSSRule {
    macro_rules! decode {
        ($($interface:ident),+ $(,)?) => {
            $(
                if let Some(object) = cell.object_if::<$interface>() {
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

/// The native rule of the rule, which the rule keeps alive.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_native(rule: &InterfaceCell) -> *const NativeRule {
    Rc::as_ptr(rule_from_abi(rule).native_rule())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_parent_rule(rule: &InterfaceCell) -> *mut c_void {
    let rule = rule_from_abi(rule);
    rule.parent_rule_object().map_or(null_mut(), AnyCSSRule::cell)
}

/// The cell of the CSSStyleSheet that the rule is in.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_parent_style_sheet(rule: &InterfaceCell) -> *mut c_void {
    let rule = rule_from_abi(rule);
    rule.parent_sheet().map_or(null_mut(), |sheet| sheet.as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_serialized(vm: &Vm, rule: &InterfaceCell) -> usize {
    let rule = rule_from_abi(rule);
    rule.serialized(vm).into_raw()
}

/// The cell of the CSSStyleDeclaration of the declarations of the rule.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_declarations(vm: &Vm, rule: &InterfaceCell) -> *mut c_void {
    let rule = rule_from_abi(rule);
    rule.declarations(vm).as_ptr()
}

/// The cell of the list of child rules of the rule.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_child_rules(vm: &Vm, rule: &InterfaceCell) -> *mut c_void {
    let rule = rule_from_abi(rule);
    cell_into_abi(rule.child_rules(vm).expect("C++ asks a rule with child rules"))
}

/// A new CSSRuleList of the native rules, whose @import rules `document` loads, if it is not null.
///
/// # Safety
///
/// `vm` must be the live VM, `rules` a live list, and `document` the live cell of a Document, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_rule_list_create(
    vm: &Vm,
    rules: *const NativeRuleList,
    document: Option<&LentCppCell<cpp_class::Document>>,
) -> *mut c_void {
    // SAFETY: The caller passes a live list.
    let (rules, document) = unsafe {
        (
            NativeRuleList::retained_from_raw(rules),
            document.map(CppCell::from_lent),
        )
    };
    cell_into_abi(allocate(vm, |defer_gc| CSSRuleList::new(defer_gc, rules, document)))
}

/// The native rules of the list, which the list keeps alive.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_list_native_rules(list: &InterfaceCell) -> *const NativeRuleList {
    Rc::as_ptr(list.implementation::<CSSRuleList>().native_rules())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_list_length(list: &InterfaceCell) -> usize {
    list.implementation::<CSSRuleList>().native_length()
}

/// The cell of the rule at the index, which has to be in the list.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_list_item(vm: &Vm, list: &InterfaceCell, index: usize) -> *mut c_void {
    let list = list.implementation::<CSSRuleList>();
    list.rule_at(vm, index).cell()
}

/// The cell of the object of the rule with the identity, if the rule has one yet.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_list_existing_rule(list: &InterfaceCell, identity: u64) -> *mut c_void {
    let list = list.implementation::<CSSRuleList>();
    list.existing_object(identity).map_or(null_mut(), AnyCSSRule::cell)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_list_set_parent_style_sheet(
    vm: &Vm,
    list: &InterfaceCell,
    sheet: Option<&LentCppCell<cpp_class::CSSStyleSheet>>,
) {
    let (list, sheet) = (list.implementation::<CSSRuleList>(), sheet.map(CppCell::from_lent));
    list.set_parent_style_sheet(vm, sheet);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_list_set_rules(
    vm: &Vm,
    list: &InterfaceCell,
    rules: &NativeRuleList,
    document: Option<&LentCppCell<cpp_class::Document>>,
) {
    let (list, document) = (list.implementation::<CSSRuleList>(), document.map(CppCell::from_lent));
    list.set_rules(vm, rules, document);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_css_rule_list_remove_rule(vm: &Vm, list: &InterfaceCell, index: usize) {
    let list = list.implementation::<CSSRuleList>();
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
    vm: &Vm,
    rule: *const NativeRule,
    import: &LentCppCell<cpp_class::StyleSheetImportCell>,
    sheet: Option<&LentCppCell<cpp_class::CSSStyleSheet>>,
) -> *mut c_void {
    // SAFETY: The caller passes a live rule.
    let (rule, import, sheet) = unsafe {
        (
            NativeRule::retained_from_raw(rule),
            CppCell::from_lent(import),
            sheet.map(CppCell::from_lent),
        )
    };
    let object = CSSImportRule::finish(CSSImportRule::create_for_import(vm, rule, import), vm);
    object.store_parent_style_sheet(vm, sheet);
    object.cell()
}

/// The cell of the StyleSheetImportCell of the rule.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_import_rule_import(rule: &InterfaceCell) -> *mut c_void {
    let rule: Ref<CSSImportRule> = rule.object();
    rule.import_cell()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_css_style_rule_set_selector_text(rule: &InterfaceCell, text: Utf16String) {
    let rule = rule.object::<CSSStyleRule>();
    rule.replace_selectors(&text);
}

/// A new CSSStyleSheet for the cell that holds the state of the sheet.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_style_sheet_create(
    vm: &Vm,
    state: &LentCppCell<cpp_class::StyleSheetStateCell>,
) -> *mut c_void {
    let state = CppCell::from_lent(state);
    cell_into_abi(CSSStyleSheet::create(vm, state))
}

/// The cell that holds the state of the sheet.
#[unsafe(no_mangle)]
pub extern "C" fn web_css_style_sheet_state(sheet: &InterfaceCell) -> *mut c_void {
    let sheet: Ref<CSSStyleSheet> = sheet.object();
    sheet.state_cell().as_ptr()
}

/// A new StyleSheetList of the sheets of `scope`, which belongs to `node`.
///
/// # Safety
///
/// `scope` must be the live StyleScope owned by the Document or ShadowRoot `node`, which keeps it alive for as long
/// as the StyleSheetList holds `node`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_sheet_list_create(
    vm: &Vm,
    scope: *mut c_void,
    node: &LentCppCell<cpp_class::Node>,
) -> *mut c_void {
    let node = CppCell::from_lent(node);
    cell_into_abi(allocate(vm, |_| StyleSheetList::new(scope, node)))
}

/// A new VisualViewport of the document.
#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_create(
    vm: &Vm,
    realm: Gc<Realm>,
    document: &LentCppCell<cpp_class::Document>,
) -> *mut c_void {
    let document = CppCell::from_lent(document);
    cell_into_abi(VisualViewport::create(vm, realm, document))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_event_target(viewport: &InterfaceCell) -> *mut c_void {
    viewport.object::<VisualViewport>().event_target().as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_offset(viewport: &InterfaceCell) -> FfiCssPixelPoint {
    viewport.object::<VisualViewport>().offset().into()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_offset_left(viewport: &InterfaceCell) -> f64 {
    viewport.object::<VisualViewport>().offset_left_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_offset_top(viewport: &InterfaceCell) -> f64 {
    viewport.object::<VisualViewport>().offset_top_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_page_left(viewport: &InterfaceCell) -> f64 {
    viewport.object::<VisualViewport>().page_left_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_page_top(viewport: &InterfaceCell) -> f64 {
    viewport.object::<VisualViewport>().page_top_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_width(viewport: &InterfaceCell) -> f64 {
    viewport.object::<VisualViewport>().width_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_height(viewport: &InterfaceCell) -> f64 {
    viewport.object::<VisualViewport>().height_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_scale(viewport: &InterfaceCell) -> f64 {
    viewport.object::<VisualViewport>().scale_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_scroll_by(viewport: &InterfaceCell, delta: FfiCssPixelPoint) {
    viewport.object::<VisualViewport>().scroll_by(delta.into());
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_zoom(viewport: &InterfaceCell, position: FfiCssPixelPoint, scale_delta: f64) {
    viewport.object::<VisualViewport>().zoom(position.into(), scale_delta);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_set_scale_zoomed_by_compositor(viewport: &InterfaceCell, scale: f64) {
    viewport
        .object::<VisualViewport>()
        .set_scale_zoomed_by_compositor(scale);
}

/// Whether the scale changed since the compositor last heard of it, in which case `scale` receives it.
#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_take_scale_for_compositor(viewport: &InterfaceCell, scale: &mut f64) -> bool {
    let taken = viewport.object::<VisualViewport>().take_scale_for_compositor();
    if let Some(taken) = taken {
        *scale = taken;
    }
    taken.is_some()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_map_to_layout_viewport(
    viewport: &InterfaceCell,
    position: FfiCssPixelPoint,
) -> FfiCssPixelPoint {
    viewport
        .object::<VisualViewport>()
        .map_to_layout_viewport(position.into())
        .into()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_map_point_from_layout_viewport(
    viewport: &InterfaceCell,
    position: FfiCssPixelPoint,
) -> FfiCssPixelPoint {
    viewport
        .object::<VisualViewport>()
        .map_point_from_layout_viewport(position.into())
        .into()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_map_rect_from_layout_viewport(
    viewport: &InterfaceCell,
    rect: FfiCssPixelRect,
) -> FfiCssPixelRect {
    let viewport = viewport.object::<VisualViewport>();
    let mapped = viewport.map_rect_from_layout_viewport(CssPixelRect::new(rect.x, rect.y, rect.width, rect.height));
    FfiCssPixelRect {
        x: mapped.x,
        y: mapped.y,
        width: mapped.width,
        height: mapped.height,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_visual_viewport_reset(viewport: &InterfaceCell) {
    viewport.object::<VisualViewport>().reset();
}
