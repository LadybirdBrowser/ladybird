/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSRule, https://drafts.csswg.org/cssom/#the-cssrule-interface

use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::{NonNull, null_mut};
use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Object, ThrowCompletionOr, Trace, Value, Vm};
use libweb_rust::{NativeRule, NativeRuleList, NativeRuleType, TextSink};

use super::css_condition_rule::CSSConditionRule;
use super::css_container_rule::CSSContainerRule;
use super::css_counter_style_rule::CSSCounterStyleRule;
use super::css_font_face_rule::CSSFontFaceRule;
use super::css_font_feature_values_rule::CSSFontFeatureValuesRule;
use super::css_function_declarations::CSSFunctionDeclarations;
use super::css_function_rule::CSSFunctionRule;
use super::css_grouping_rule::CSSGroupingRule;
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
use super::css_rule_list::CSSRuleList;
use super::css_scope_rule::CSSScopeRule;
use super::css_style_rule::CSSStyleRule;
use super::css_style_sheet::CSSStyleSheet;
use super::css_supports_rule::CSSSupportsRule;
use crate::bindings::conversions::{CppInterface, ToJs};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Interface, Ref, Wrappable, cell_from_abi, cell_into_abi, wrap};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_wrap_object(realm: *mut c_void, cell: *mut c_void) -> *mut c_void;
    fn web_css_rule_create_declarations(rule: *mut c_void) -> *mut c_void;
    fn web_css_declarations_serialized(declarations: *mut c_void) -> usize;
    fn web_css_declaration_owner(declaration: *mut c_void) -> *mut c_void;
    fn web_css_style_sheet_native_rules(sheet: *mut c_void) -> *const c_void;
    fn web_css_selector_list_text(selectors: *const c_void, sheet: *mut c_void) -> usize;
}

/// The wrapper of the object of an interface implemented in C++ in the world of the call.
pub(crate) fn wrap_cpp_cell(cx: &CallContext, cell: &CppCell) -> CppInterface {
    // SAFETY: The realm is live, and the cell is that of an object of an interface implemented in C++.
    let wrapper = unsafe { web_css_wrap_object(cx.realm().as_ptr().cast(), cell.as_ptr()) };
    let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns the wrapper of the object");
    // SAFETY: LibWeb returns a live wrapper of the object.
    unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
}

/// The text of the selector list that the pointer points to, which names the namespaces of the sheet.
pub(crate) fn selector_list_text(selectors: *const c_void, sheet: Option<&CppCell>) -> Utf16String {
    let sheet = sheet.map_or(null_mut(), CppCell::as_ptr);
    // SAFETY: The caller passes a live list, and the sheet is the live cell of a CSSStyleSheet, or null.
    let text = unsafe { web_css_selector_list_text(selectors, sheet) };
    // SAFETY: LibWeb hands over the string.
    unsafe { Utf16String::from_raw_owned(text) }
}

/// The native rules of the sheet, whose cell is that of a CSSStyleSheet.
pub(crate) fn native_rules_of_sheet(sheet: &CppCell) -> Rc<NativeRuleList> {
    // SAFETY: The cell is that of a live CSSStyleSheet.
    let rules = unsafe { web_css_style_sheet_native_rules(sheet.as_ptr()) };
    // SAFETY: LibWeb returns the live list of the sheet.
    unsafe { NativeRuleList::retained_from_raw(rules.cast()) }
}

/// What an interface that inherits from CSSRule says about the memory of its objects outside the heap, which is that
/// of the native rule.
macro_rules! impl_rule_wrappable {
    ($type:ty) => {
        impl crate::bindings::interface::Wrappable for $type {
            const OWNS_EXTERNAL_MEMORY: bool = true;

            fn external_memory_size(&self) -> usize {
                self.rule().native_rule().cssom_external_memory_size()
            }
        }
    };
}
pub(crate) use impl_rule_wrappable;

/// A rule's part of every rule, which the attributes of CSSRule run on.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSRule {
    /// The rule of the sheet that the object represents.
    #[gc(untraced)]
    native_rule: Rc<NativeRule>,
    /// The object that this is the part of, which the steps that differ between the interfaces run on.
    object: GcRefCell<Option<AnyCSSRule>>,
    /// https://drafts.csswg.org/cssom/#concept-css-rule-parent-css-rule
    parent_rule: GcRefCell<Option<AnyCSSRule>>,
    /// https://drafts.csswg.org/cssom/#concept-css-rule-parent-css-style-sheet, the cell of a CSSStyleSheet.
    parent_style_sheet: GcRefCell<Option<CppCell>>,
    /// The cell of the CSSStyleDeclaration of the declarations of the rule, once it is asked for.
    declarations: GcRefCell<Option<CppCell>>,
}

impl Wrappable for CSSRule {}

impl CSSRule {
    pub(crate) fn new(native_rule: Rc<NativeRule>) -> Self {
        Self {
            native_rule,
            object: GcRefCell::new(None),
            parent_rule: GcRefCell::new(None),
            parent_style_sheet: GcRefCell::new(None),
            declarations: GcRefCell::new(None),
        }
    }

    pub(crate) fn native_rule(&self) -> &Rc<NativeRule> {
        &self.native_rule
    }

    pub(crate) fn set_object(&self, vm: &Vm, object: AnyCSSRule) {
        *self.object.borrow_mut(vm) = Some(object);
    }

    pub(crate) fn object(&self) -> AnyCSSRule {
        self.object.borrow().expect("an object knows what it is")
    }

    pub(crate) fn parent_rule_object(&self) -> Option<AnyCSSRule> {
        *self.parent_rule.borrow()
    }

    pub(crate) fn parent_sheet(&self) -> Option<CppCell> {
        self.parent_style_sheet.borrow().clone()
    }

    pub(crate) fn store_parent_rule(&self, vm: &Vm, parent_rule: Option<AnyCSSRule>) {
        *self.parent_rule.borrow_mut(vm) = parent_rule;
    }

    pub(crate) fn store_parent_style_sheet(&self, vm: &Vm, parent_style_sheet: Option<CppCell>) {
        *self.parent_style_sheet.borrow_mut(vm) = parent_style_sheet;
    }

    /// The CSSStyleDeclaration of the declarations of the rule, which the first call creates.
    pub(crate) fn declarations(&self, vm: &Vm) -> CppCell {
        if let Some(declarations) = self.declarations.borrow().as_ref() {
            return declarations.clone();
        }
        // SAFETY: The cell is that of this object, which is live.
        let declarations = unsafe { web_css_rule_create_declarations(self.object().cell()) };
        // SAFETY: LibWeb returns a live cell.
        let declarations = unsafe { CppCell::from_raw(declarations) };
        *self.declarations.borrow_mut(vm) = Some(declarations.clone());
        declarations
    }

    /// The text of the declarations of the rule, which is what the CSSStyleDeclaration of the rule serializes to.
    pub(crate) fn serialized_declarations(&self, vm: &Vm) -> Utf16String {
        let declarations = self.declarations(vm);
        // SAFETY: The cell is that of a live CSSStyleDeclaration.
        let text = unsafe { web_css_declarations_serialized(declarations.as_ptr()) };
        // SAFETY: LibWeb hands over the string.
        unsafe { Utf16String::from_raw_owned(text) }
    }

    /// The object of an interface implemented in Rust that owns the CSSStyleDeclaration of the rule, which the `style`
    /// attribute of the rule returns.
    pub(crate) fn declarations_object<T: Interface>(&self, cx: &CallContext) -> Ref<T> {
        let declarations = self.declarations(cx.vm());
        // SAFETY: The cell is that of a live CSSStyleDeclaration, whose owner is an object of the interface of the rule.
        unsafe { cell_from_abi(web_css_declaration_owner(declarations.as_ptr())) }
    }

    // https://drafts.csswg.org/cssom/#dom-cssrule-csstext
    pub(crate) fn css_text(&self, cx: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The cssText attribute must return a serialization of the CSS rule.
        Ok(self.object().serialized(cx.vm()))
    }

    // https://drafts.csswg.org/cssom/#dom-cssrule-csstext
    pub(crate) fn set_css_text(&self, _: &mut CallContext, _: Utf16String) -> WebIdlResult<()> {
        // On setting the cssText attribute must do nothing.
        Ok(())
    }

    // https://drafts.csswg.org/cssom/#dom-cssrule-parentrule
    pub(crate) fn parent_rule(&self, _: &mut CallContext) -> WebIdlResult<Option<AnyCSSRule>> {
        Ok(self.parent_rule_object())
    }

    // https://drafts.csswg.org/cssom/#dom-cssrule-parentstylesheet
    pub(crate) fn parent_style_sheet(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<CSSStyleSheet>>> {
        let sheet = self.parent_style_sheet.borrow();
        // SAFETY: The cell is that of a live CSSStyleSheet.
        Ok(sheet.as_ref().map(|sheet| unsafe { cell_from_abi(sheet.as_ptr()) }))
    }

    // https://drafts.csswg.org/cssom/#dom-cssrule-type
    pub(crate) fn type_for_bindings(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        // NB: Types that aren't defined in the spec must return 0.
        //     To do this, we arbitrarily make non-spec ones start at 100.
        let rule_type = self.native_rule.cssom_type() as u16;
        Ok(if rule_type >= 100 { 0 } else { rule_type })
    }
}

/// The object of a CSSRule, of an interface that inherits from it.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
#[expect(clippy::enum_variant_names, reason = "the variants are named after the interfaces")]
#[expect(
    dead_code,
    reason = "the bindings match the variants of interfaces that only others inherit from"
)]
pub(crate) enum AnyCSSRule {
    CSSRule(Ref<CSSRule>),
    CSSConditionRule(Ref<CSSConditionRule>),
    CSSContainerRule(Ref<CSSContainerRule>),
    CSSCounterStyleRule(Ref<CSSCounterStyleRule>),
    CSSFontFaceRule(Ref<CSSFontFaceRule>),
    CSSFontFeatureValuesRule(Ref<CSSFontFeatureValuesRule>),
    CSSFunctionDeclarations(Ref<CSSFunctionDeclarations>),
    CSSFunctionRule(Ref<CSSFunctionRule>),
    CSSGroupingRule(Ref<CSSGroupingRule>),
    CSSImportRule(Ref<CSSImportRule>),
    CSSKeyframeRule(Ref<CSSKeyframeRule>),
    CSSKeyframesRule(Ref<CSSKeyframesRule>),
    CSSLayerBlockRule(Ref<CSSLayerBlockRule>),
    CSSLayerStatementRule(Ref<CSSLayerStatementRule>),
    CSSMarginRule(Ref<CSSMarginRule>),
    CSSMediaRule(Ref<CSSMediaRule>),
    CSSNamespaceRule(Ref<CSSNamespaceRule>),
    CSSNestedDeclarations(Ref<CSSNestedDeclarations>),
    CSSPageRule(Ref<CSSPageRule>),
    CSSPositionTryRule(Ref<CSSPositionTryRule>),
    CSSPropertyRule(Ref<CSSPropertyRule>),
    CSSScopeRule(Ref<CSSScopeRule>),
    CSSStyleRule(Ref<CSSStyleRule>),
    CSSSupportsRule(Ref<CSSSupportsRule>),
}

/// Runs the expression on the object of every interface that has objects of its own.
macro_rules! for_each_rule {
    ($any:expr, $object:ident => $expression:expr) => {
        match $any {
            AnyCSSRule::CSSContainerRule($object) => $expression,
            AnyCSSRule::CSSCounterStyleRule($object) => $expression,
            AnyCSSRule::CSSFontFaceRule($object) => $expression,
            AnyCSSRule::CSSFontFeatureValuesRule($object) => $expression,
            AnyCSSRule::CSSFunctionDeclarations($object) => $expression,
            AnyCSSRule::CSSFunctionRule($object) => $expression,
            AnyCSSRule::CSSImportRule($object) => $expression,
            AnyCSSRule::CSSKeyframeRule($object) => $expression,
            AnyCSSRule::CSSKeyframesRule($object) => $expression,
            AnyCSSRule::CSSLayerBlockRule($object) => $expression,
            AnyCSSRule::CSSLayerStatementRule($object) => $expression,
            AnyCSSRule::CSSMarginRule($object) => $expression,
            AnyCSSRule::CSSMediaRule($object) => $expression,
            AnyCSSRule::CSSNamespaceRule($object) => $expression,
            AnyCSSRule::CSSNestedDeclarations($object) => $expression,
            AnyCSSRule::CSSPageRule($object) => $expression,
            AnyCSSRule::CSSPositionTryRule($object) => $expression,
            AnyCSSRule::CSSPropertyRule($object) => $expression,
            AnyCSSRule::CSSScopeRule($object) => $expression,
            AnyCSSRule::CSSStyleRule($object) => $expression,
            AnyCSSRule::CSSSupportsRule($object) => $expression,
            AnyCSSRule::CSSRule(_) | AnyCSSRule::CSSGroupingRule(_) | AnyCSSRule::CSSConditionRule(_) => {
                unreachable!("nothing creates an object of an interface that only others inherit from")
            }
        }
    };
}

impl Deref for AnyCSSRule {
    type Target = CSSRule;

    fn deref(&self) -> &CSSRule {
        for_each_rule!(self, object => object.rule())
    }
}

impl ToJs for AnyCSSRule {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(for_each_rule!(self, object => wrap(cx, object))))
    }
}

impl AnyCSSRule {
    /// The object for a rule of a sheet, which `document` loads what it imports for.
    pub(crate) fn create(vm: &Vm, rule: Rc<NativeRule>, document: Option<&CppCell>) -> Self {
        let object = match rule.cssom_type() {
            NativeRuleType::Style => Self::CSSStyleRule(CSSStyleRule::create(vm, rule)),
            NativeRuleType::Import => Self::CSSImportRule(CSSImportRule::create(vm, rule, document)),
            NativeRuleType::Media => Self::CSSMediaRule(CSSMediaRule::create(vm, rule)),
            NativeRuleType::FontFace => Self::CSSFontFaceRule(CSSFontFaceRule::create(vm, rule)),
            NativeRuleType::Page => Self::CSSPageRule(CSSPageRule::create(vm, rule)),
            NativeRuleType::Keyframes => Self::CSSKeyframesRule(CSSKeyframesRule::create(vm, rule)),
            NativeRuleType::Keyframe => Self::CSSKeyframeRule(CSSKeyframeRule::create(vm, rule)),
            NativeRuleType::Margin => Self::CSSMarginRule(CSSMarginRule::create(vm, rule)),
            NativeRuleType::Namespace => Self::CSSNamespaceRule(CSSNamespaceRule::create(vm, rule)),
            NativeRuleType::CounterStyle => Self::CSSCounterStyleRule(CSSCounterStyleRule::create(vm, rule)),
            NativeRuleType::Supports => Self::CSSSupportsRule(CSSSupportsRule::create(vm, rule)),
            NativeRuleType::FontFeatureValues => {
                Self::CSSFontFeatureValuesRule(CSSFontFeatureValuesRule::create(vm, rule))
            }
            NativeRuleType::LayerBlock => Self::CSSLayerBlockRule(CSSLayerBlockRule::create(vm, rule)),
            NativeRuleType::LayerStatement => Self::CSSLayerStatementRule(CSSLayerStatementRule::create(vm, rule)),
            NativeRuleType::NestedDeclarations => Self::CSSNestedDeclarations(CSSNestedDeclarations::create(vm, rule)),
            NativeRuleType::Property => Self::CSSPropertyRule(CSSPropertyRule::create(vm, rule)),
            NativeRuleType::Function => Self::CSSFunctionRule(CSSFunctionRule::create(vm, rule)),
            NativeRuleType::FunctionDeclarations => {
                Self::CSSFunctionDeclarations(CSSFunctionDeclarations::create(vm, rule))
            }
            NativeRuleType::Container => Self::CSSContainerRule(CSSContainerRule::create(vm, rule)),
            NativeRuleType::Scope => Self::CSSScopeRule(CSSScopeRule::create(vm, rule)),
            NativeRuleType::PositionTry => Self::CSSPositionTryRule(CSSPositionTryRule::create(vm, rule)),
        };
        if let Self::CSSImportRule(import) = object {
            return CSSImportRule::finish(import, vm);
        }
        object.set_object(vm, object);
        object
    }

    /// The cell of the object, which C++ names through the handles of the rules.
    pub(crate) fn cell(self) -> *mut c_void {
        for_each_rule!(self, object => cell_into_abi(object))
    }

    /// The object as a rule with child rules that it manages itself, if it is one.
    pub(crate) fn grouping_rule(&self) -> Option<&CSSGroupingRule> {
        match self {
            Self::CSSContainerRule(object) => Some(object.grouping_rule()),
            Self::CSSFunctionRule(object) => Some(object.grouping_rule()),
            Self::CSSLayerBlockRule(object) => Some(object.grouping_rule()),
            Self::CSSMediaRule(object) => Some(object.grouping_rule()),
            Self::CSSPageRule(object) => Some(object.grouping_rule()),
            Self::CSSScopeRule(object) => Some(object.grouping_rule()),
            Self::CSSStyleRule(object) => Some(object.grouping_rule()),
            Self::CSSSupportsRule(object) => Some(object.grouping_rule()),
            _ => None,
        }
    }

    /// The list of child rules of the object, if it has made one.
    pub(crate) fn existing_child_rules(&self) -> Option<Ref<CSSRuleList>> {
        if let Self::CSSKeyframesRule(object) = self {
            return object.existing_css_rules();
        }
        self.grouping_rule().and_then(CSSGroupingRule::existing_css_rules)
    }

    /// The list of child rules of the object, which the first call makes.
    pub(crate) fn child_rules(self, vm: &Vm) -> Option<Ref<CSSRuleList>> {
        if let Self::CSSKeyframesRule(object) = self {
            return Some(object.child_rules(vm));
        }
        self.grouping_rule().map(|rule| rule.child_rules(vm))
    }

    /// https://drafts.csswg.org/cssom/#serialize-a-css-rule
    pub(crate) fn serialized(self, vm: &Vm) -> Utf16String {
        for_each_rule!(self, object => object.serialized(vm))
    }

    pub(crate) fn set_parent_rule(self, vm: &Vm, parent_rule: Option<Self>) {
        self.store_parent_rule(vm, parent_rule);
        self.set_parent_style_sheet(vm, parent_rule.and_then(|parent_rule| parent_rule.parent_sheet()));
    }

    pub(crate) fn set_parent_style_sheet(self, vm: &Vm, sheet: Option<CppCell>) {
        self.store_parent_style_sheet(vm, sheet.clone());
        if let Self::CSSImportRule(object) = self {
            object.parent_style_sheet_changed(sheet.as_ref());
        }
        if let Some(rules) = self.existing_child_rules() {
            rules.set_parent_style_sheet(vm, sheet);
        }
    }
}

/// The text of each of the rules of the list, without the empty ones.
pub(crate) fn serialized_rules(vm: &Vm, rules: Ref<CSSRuleList>) -> Vec<Utf16String> {
    (0..rules.native_length())
        .map(|index| rules.rule_at(vm, index).serialized(vm))
        .filter(|text| !text.is_empty())
        .collect()
}

/// A string that grows by what the rules serialize to.
#[derive(Default)]
pub(crate) struct RuleText(TextSink);

impl RuleText {
    pub(crate) fn new(text: &str) -> Self {
        let mut result = Self::default();
        result.push_str(text);
        result
    }

    pub(crate) fn push_str(&mut self, text: &str) {
        text.encode_utf16().for_each(|unit| self.0.push_code_unit(unit));
    }

    pub(crate) fn push(&mut self, text: &Utf16String) {
        self.0.push_string(text);
    }

    pub(crate) fn push_units(&mut self, units: &[u16]) {
        units.iter().for_each(|&unit| self.0.push_code_unit(unit));
    }

    pub(crate) fn finish(self) -> Utf16String {
        self.0.into_utf16_string()
    }
}

/// `@<at-rule> {`, each rule on a line of its own indented by two spaces, and `}`, as @media, @scope and @container
/// serialize, https://drafts.csswg.org/cssom/#serialize-a-css-rule
pub(crate) fn serialize_block_of_rules(mut text: RuleText, vm: &Vm, rules: Ref<CSSRuleList>) -> Utf16String {
    // 3. A single SPACE (U+0020), followed by the string "{", i.e., LEFT CURLY BRACKET (U+007B), followed by a newline.
    text.push_str(" {\n");

    // 4. The result of performing serialize a CSS rule on each rule in the rule’s cssRules list,
    //    filtering out empty strings, indenting each item with two spaces, all joined with newline.
    for rule in serialized_rules(vm, rules) {
        text.push_str("  ");
        text.push(&rule);
        text.push_str("\n");
    }

    // 5. A newline, followed by the string "}", i.e., RIGHT CURLY BRACKET (U+007D)
    // AD-HOC: All modern browsers omit the ending newline if there are no CSS rules, so let's do the same.
    //         If there are rules, the required newline is appended in the loop above.
    text.push_str("}");
    text.finish()
}
