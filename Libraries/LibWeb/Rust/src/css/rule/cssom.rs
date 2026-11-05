/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the CSSOM reads and changes of a rule, for libweb_bindings_rust, whose interfaces are implemented in Rust.

use std::rc::Rc;
use std::sync::Arc;

use ak::Utf16String;

use super::{NativeRule, NativeRuleList, NativeRuleType, RuleInsertionResult, RulePayload, RuleRemovalResult};
use crate::css::css_string::CssString;
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::descriptor_block::FfiDescriptorBlock;
use crate::css::descriptor_metadata::descriptor_metadata;
use crate::css::font_feature_values::FontFeatureValuesRuleKind;
use crate::css::parser::descriptor_parser::parse_descriptor_text;
use crate::css::parser::query_parser::{FfiQueryHandle, serialized_query_condition, supports_query_matches};
use crate::css::parser::syntax::SyntaxNode;
use crate::css::parser::syntax_parser::{
    PageSelectorList, parse_keyframe_selector_list, parse_page_selector_list, serialized_page_selectors,
};
use crate::css::serialize::{
    SerializationMode, StringUnits, TextSink, serialize_a_string, serialize_an_identifier, serialize_style_value,
};
use crate::css::style_value_arc::StyleValueArc;

/// The at-rule id of @counter-style, as the descriptors of the generated metadata know it.
const COUNTER_STYLE_AT_RULE: u8 = 3;

fn utf16(units: &[u16]) -> Utf16String {
    Utf16String::from_utf16(units)
}

fn css_utf16(string: &CssString) -> Utf16String {
    utf16(string.units())
}

fn string_units(string: &Utf16String) -> StringUnits<'_> {
    match string.as_units() {
        ak::Utf16StringUnits::Ascii(bytes) | ak::Utf16StringUnits::Latin1(bytes) => StringUnits::Ascii(bytes),
        ak::Utf16StringUnits::Utf16(units) => StringUnits::Utf16(units),
    }
}

/// https://drafts.csswg.org/cssom/#serialize-an-identifier
pub fn serialize_identifier(identifier: &Utf16String) -> Utf16String {
    let mut sink = TextSink::new();
    serialize_an_identifier(&mut sink, &string_units(identifier));
    sink.into_utf16_string()
}

/// https://drafts.csswg.org/cssom/#serialize-a-string
pub fn serialize_string_value(string: &Utf16String) -> Utf16String {
    let mut sink = TextSink::new();
    serialize_a_string(&mut sink, &string_units(string));
    sink.into_utf16_string()
}

fn value_text(value: &StyleValueArc) -> Utf16String {
    let mut sink = TextSink::new();
    serialize_style_value(&mut sink, value, SerializationMode::Normal);
    sink.into_utf16_string()
}

fn syntax_text(syntax: &SyntaxNode) -> Utf16String {
    let mut sink = TextSink::new();
    syntax.serialize_to(&mut sink);
    sink.into_utf16_string()
}

/// https://drafts.csswg.org/css-mixins-1/#serialize-a-css-type
fn serialize_a_css_type(sink: &mut TextSink, syntax: &SyntaxNode) {
    // To serialize a CSS type, return the concatenation of the following:
    // If the <css-type> consists of a single <syntax-component>, return the corresponding syntax string.
    if syntax.is_single_component() {
        syntax.serialize_to(sink);
        return;
    }

    // Otherwise, return the concatenation of the following:
    // The string "type(", i.e. "type" followed by a single LEFT PARENTHESIS (U+0028).
    sink.push_ascii("type(");

    // The corresponding syntax string.
    syntax.serialize_to(sink);

    // The string ")", i.e. a single RIGHT PARENTHESIS (U+0029).
    sink.push_ascii(")");
}

/// The name, type and default value of a parameter of a custom function, https://drafts.csswg.org/css-mixins-1/#dictdef-functionparameter
pub struct FunctionParameter {
    pub name: Utf16String,
    pub parameter_type: Utf16String,
    pub default_value: Option<Utf16String>,
}

/// The name and the query of a <container-condition>, each empty when the condition does not have it.
pub struct ContainerCondition {
    pub name: Utf16String,
    pub query: Utf16String,
}

/// The kinds of feature values of a @font-feature-values rule, in the order of the rule's serialization.
pub const FONT_FEATURE_VALUES_KINDS: [(FontFeatureValuesRuleKind, &str); 7] = [
    (FontFeatureValuesRuleKind::Annotation, "annotation"),
    (FontFeatureValuesRuleKind::Ornaments, "ornaments"),
    (FontFeatureValuesRuleKind::Stylistic, "stylistic"),
    (FontFeatureValuesRuleKind::Swash, "swash"),
    (FontFeatureValuesRuleKind::CharacterVariant, "character-variant"),
    (FontFeatureValuesRuleKind::Styleset, "styleset"),
    (FontFeatureValuesRuleKind::HistoricalForms, "historical-forms"),
];

/// Whether the name of a @keyframes rule serializes as an identifier, and not as a string.
pub fn is_valid_animation_name(name: &Utf16String) -> bool {
    crate::css::parser::value_parser::is_valid_animation_name(name.as_units().into())
}

impl NativeRule {
    /// The rule that the pointer points to, which keeps its reference.
    ///
    /// # Safety
    ///
    /// `rule` must point to a live rule that an `Rc` owns.
    pub unsafe fn retained_from_raw(rule: *const Self) -> Rc<Self> {
        // SAFETY: The caller passes a pointer into a live Rc.
        unsafe {
            Rc::increment_strong_count(rule);
            Rc::from_raw(rule)
        }
    }

    pub fn cssom_identity(&self) -> u64 {
        self.identity
    }

    pub fn cssom_type(&self) -> NativeRuleType {
        self.rule_type
    }

    /// The list of child rules of a rule that has one, which a style rule makes when it is asked.
    pub fn cssom_children(&self) -> Option<Rc<NativeRuleList>> {
        self.child_rules().cloned()
    }

    /// The memory the rule owns outside the heap.
    pub fn cssom_external_memory_size(&self) -> usize {
        super::rule_external_memory_size(self)
    }

    pub fn cssom_set_selector_text(&self, text: &Utf16String, sheet_rules: Option<&NativeRuleList>) -> bool {
        self.set_selector_text(text.as_units().into(), sheet_rules)
    }

    /// Whether the rule has an empty block of declarations or descriptors.
    pub fn has_no_declarations(&self) -> bool {
        match &self.payload {
            RulePayload::FontFace(block)
            | RulePayload::FunctionDeclarations(block)
            | RulePayload::Page { descriptors: block, .. } => block.data().descriptors.is_empty(),
            _ => self.declaration_block().is_none_or(|block| block.data().is_empty()),
        }
    }

    fn descriptor_block(&self) -> &FfiDescriptorBlock {
        match &self.payload {
            RulePayload::FontFace(block)
            | RulePayload::FunctionDeclarations(block)
            | RulePayload::Page { descriptors: block, .. } => block,
            RulePayload::CounterStyle(rule) => rule.descriptor_block(),
            _ => unreachable!("expected a rule with descriptors"),
        }
    }

    /// The value of the descriptor with the name, serialized.
    pub fn descriptor_text(&self, at_rule: u8, name: &str) -> Option<Utf16String> {
        let name: Vec<u16> = name.encode_utf16().collect();
        let id = descriptor_metadata(at_rule, &name)?.id;
        let data = self.descriptor_block().data();
        let descriptor = data.descriptors.iter().find(|descriptor| descriptor.id == id)?;
        Some(value_text(&descriptor.value))
    }

    /// https://drafts.csswg.org/css-animations-1/#dom-csskeyframesrule-name
    pub fn keyframes_name(&self) -> Utf16String {
        let RulePayload::Keyframes(name) = &self.payload else {
            unreachable!("expected a @keyframes rule")
        };
        css_utf16(&name.borrow())
    }

    pub fn set_keyframes_name(&self, name: &Utf16String) {
        let RulePayload::Keyframes(current) = &self.payload else {
            unreachable!("expected a @keyframes rule")
        };
        *current.borrow_mut() = CssString::from_utf16(&name.to_utf16());
    }

    /// The index of the last keyframe with the selector.
    pub fn find_keyframe(&self, selector: &Utf16String) -> Option<usize> {
        let selectors = parse_keyframe_selector_list(selector.as_units().into())?;
        let matches = |frame: &Self| {
            let RulePayload::Keyframe(frame) = &frame.payload else {
                unreachable!("a @keyframes rule holds keyframes")
            };
            frame.keys() == selectors
        };
        let children = self.child_rules()?;
        if let Some(rules) = children.rules.borrow().as_ref() {
            return rules.iter().rposition(|frame| matches(frame));
        }
        // NB: Most keyframes are not exposed yet, and reading the parsed source leaves them that way.
        let parsed = children.parsed_source.borrow();
        let parsed = parsed.as_ref()?;
        let exposed = children.exposed_rules.borrow();
        (0..parsed.len()).rev().find(|index| match exposed.get(index) {
            Some(frame) => matches(frame),
            None => *parsed.rule_at(*index).keyframe().unwrap().keys == selectors,
        })
    }

    pub fn keyframe_keys(&self) -> Vec<f64> {
        let RulePayload::Keyframe(frame) = &self.payload else {
            unreachable!("expected a keyframe rule")
        };
        frame.keys().to_vec()
    }

    pub fn page_selector_text(&self) -> Utf16String {
        let RulePayload::Page { selectors, .. } = &self.payload else {
            unreachable!("expected a @page rule")
        };
        serialized_page_selectors(&selectors.borrow())
    }

    /// Replaces the selectors of a @page rule with what the text parses to, and returns whether it parsed.
    pub fn set_page_selector_text(&self, text: &Utf16String) -> bool {
        let RulePayload::Page { selectors, .. } = &self.payload else {
            unreachable!("expected a @page rule")
        };
        let Some(parsed) = parse_page_selector_list(TokenizerInput::from(text.as_units())) else {
            return false;
        };
        *selectors.borrow_mut() = Arc::new(PageSelectorList::new(parsed));
        true
    }

    /// The name of a margin rule or of a @position-try rule.
    pub fn at_rule_name(&self) -> Utf16String {
        match &self.payload {
            RulePayload::Margin { name, .. } | RulePayload::PositionTry { name, .. } => css_utf16(name),
            _ => unreachable!("expected a margin or @position-try rule"),
        }
    }

    pub fn namespace_prefix(&self) -> Utf16String {
        let RulePayload::Namespace(data) = &self.payload else {
            unreachable!("expected a @namespace rule")
        };
        css_utf16(&data.prefix)
    }

    pub fn namespace_uri(&self) -> Utf16String {
        let RulePayload::Namespace(data) = &self.payload else {
            unreachable!("expected a @namespace rule")
        };
        css_utf16(&data.uri)
    }

    pub fn layer_names(&self) -> Vec<Utf16String> {
        let RulePayload::LayerNames(names) = &self.payload else {
            unreachable!("expected a @layer rule")
        };
        names.names.iter().map(css_utf16).collect()
    }

    fn property(&self) -> &crate::css::property_rule::PropertyRuleData {
        let RulePayload::Property(data) = &self.payload else {
            unreachable!("expected a @property rule")
        };
        data
    }

    pub fn property_name(&self) -> Utf16String {
        css_utf16(&self.property().name)
    }

    pub fn property_syntax(&self) -> Utf16String {
        css_utf16(&self.property().syntax_source)
    }

    pub fn property_inherits(&self) -> bool {
        self.property().inherits
    }

    pub fn property_initial_value(&self) -> Option<Utf16String> {
        self.property().initial_value.as_ref().map(value_text)
    }

    fn function(&self) -> &crate::css::function_signature::FunctionSignature {
        let RulePayload::Function(signature) = &self.payload else {
            unreachable!("expected a @function rule")
        };
        signature
    }

    pub fn function_name(&self) -> Utf16String {
        css_utf16(&self.function().name)
    }

    // https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-getparameters
    pub fn function_parameters(&self) -> Vec<FunctionParameter> {
        self.function()
            .parameters
            .iter()
            .map(|parameter| FunctionParameter {
                // name
                // The name of the function parameter.
                name: Utf16String::from_utf16(&parameter.name.to_utf16()),

                // type
                // The type of the function parameter, represented as a syntax string, or "*" if the parameter has no
                // type.
                parameter_type: syntax_text(&parameter.syntax),

                // defaultValue
                // The default value of the function parameter, or `null` if the argument does not have a default.
                default_value: parameter.default_value.as_ref().map(value_text),
            })
            .collect()
    }

    // https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-returntype
    pub fn function_return_type(&self) -> Utf16String {
        // The return type of the custom function, represented as a syntax string. If the custom function has no return
        // type, returns "*".
        // NB: We always store a return type (defaulting to "*")
        syntax_text(&self.function().return_type)
    }

    /// The text of the prelude of a @function rule, as the steps 1 to 5 of
    /// https://drafts.csswg.org/css-mixins-1/#serialize-a-cssfunctionrule make it.
    pub fn function_prelude_text(&self) -> Utf16String {
        let signature = self.function();
        // To serialize a CSSFunctionRule, return the concatenation of the following:
        let mut sink = TextSink::new();

        // 1. The string "@function" followed by a single SPACE (U+0020).
        sink.push_ascii("@function ");

        // 2. The result of performing serialize an identifier on the name of the custom function, followed by a single
        //    LEFT PARENTHESIS (U+0028).
        serialize_an_identifier(&mut sink, &StringUnits::Utf16(signature.name.units()));
        sink.push_ascii("(");

        // 3. The result of serialize a function parameter on each of the custom function’s parameters, all joined by
        //    ", " (COMMA U+002C, followed by a single SPACE U+0020).
        for (index, parameter) in signature.parameters.iter().enumerate() {
            if index != 0 {
                sink.push_ascii(", ");
            }

            // https://drafts.csswg.org/css-mixins-1/#serialize-a-function-parameter
            // To serialize a function parameter, return the concatenation of the following:

            // The result of performing serialize an identifier on the name of the function parameter.
            serialize_an_identifier(&mut sink, &StringUnits::Utf16(&parameter.name.to_utf16()));

            // If the function parameter has a type, and that type is not the universal syntax definition:
            if !parameter.syntax.is_universal() {
                // - A single SPACE (U+0020), followed by the result of performing serialize a CSS type on that type.
                sink.push_ascii(" ");
                serialize_a_css_type(&mut sink, &parameter.syntax);
            }

            // If the function parameter has a default value:
            if let Some(default_value) = &parameter.default_value {
                // - A single COLON (U+003A), followed by a single SPACE (U+0020), followed by the result of performing
                //   serialize a CSS value on that value.
                sink.push_ascii(": ");
                serialize_style_value(&mut sink, default_value, SerializationMode::Normal);
            }
        }

        // 4. A single RIGHT PARENTHESIS (U+0029).
        sink.push_ascii(")");

        // 5. If the custom function has return type, and that return type is not the universal syntax definition ("*"):
        if !signature.return_type.is_universal() {
            // - A single SPACE (U+0020), followed by the string "returns", followed by a single SPACE (U+0020).
            sink.push_ascii(" returns ");

            // - The result of performing serialize a CSS type on that type.
            serialize_a_css_type(&mut sink, &signature.return_type);
        }
        sink.into_utf16_string()
    }

    fn supports_handle(&self) -> &FfiQueryHandle {
        let RulePayload::Supports(handle) = &self.payload else {
            unreachable!("expected a @supports rule")
        };
        handle
    }

    pub fn supports_condition_text(&self) -> Utf16String {
        serialized_query_condition(self.supports_handle())
    }

    pub fn supports_condition_matches(&self) -> bool {
        supports_query_matches(self.supports_handle())
    }

    pub fn import_supports_text(&self) -> Option<Utf16String> {
        let RulePayload::Import { data, .. } = &self.payload else {
            unreachable!("expected an @import rule")
        };
        data.supports
            .as_ref()
            .map(|supports| serialized_query_condition(supports))
    }

    pub fn import_layer(&self) -> Option<Utf16String> {
        let RulePayload::Import { data, .. } = &self.payload else {
            unreachable!("expected an @import rule")
        };
        data.layer.as_ref().map(css_utf16)
    }

    /// The conditions of a @container rule, in the order of the rule.
    pub fn container_conditions(&self) -> Vec<ContainerCondition> {
        let RulePayload::Container(data) = &self.payload else {
            unreachable!("expected a @container rule")
        };
        data.conditions
            .iter()
            .map(|condition| ContainerCondition {
                name: condition
                    .name
                    .as_ref()
                    .map_or_else(|| utf16(&[]), |name| serialize_identifier(&css_utf16(name))),
                query: condition
                    .query
                    .as_ref()
                    .map_or_else(|| utf16(&[]), |query| serialized_query_condition(query)),
            })
            .collect()
    }

    /// The media queries of a rule with a list of them, which the MediaList of the rule shares.
    pub fn media_list(&self) -> crate::NativeMediaList {
        match &self.payload {
            RulePayload::Media { list } | RulePayload::Import { media: list, .. } => list.clone(),
            _ => unreachable!("expected a @media or @import rule"),
        }
    }

    /// The selector list of a style rule, as a pointer that C++ turns into text.
    pub fn style_selector_list(&self) -> *const core::ffi::c_void {
        Arc::as_ptr(&self.style_rule().selectors()).cast()
    }

    /// The selector lists that a @scope or a scoped @import rule starts and ends with, as pointers that C++ turns into
    /// text. A list that the rule omits is null.
    pub fn scope_selector_lists(&self) -> Option<(*const core::ffi::c_void, *const core::ffi::c_void)> {
        let scope = match &self.payload {
            RulePayload::Scope(scope) => Arc::clone(&scope.borrow()),
            RulePayload::Import { scope: Some(scope), .. } => Arc::clone(scope),
            _ => return None,
        };
        let pointer = |list: &Option<Arc<crate::css::selector_parser::RustParsedSelectorList>>| {
            list.as_ref().map_or(core::ptr::null(), |list| Arc::as_ptr(list).cast())
        };
        Some((pointer(&scope.start), pointer(&scope.end)))
    }

    fn counter_style(&self) -> &crate::css::counter_style::FfiCounterStyle {
        let RulePayload::CounterStyle(rule) = &self.payload else {
            unreachable!("expected a @counter-style rule")
        };
        rule
    }

    pub fn counter_style_name(&self) -> Utf16String {
        self.counter_style().name_text()
    }

    /// Sets the name of a @counter-style rule, and returns whether the name is valid.
    pub fn set_counter_style_name(&self, name: &Utf16String) -> bool {
        self.counter_style().set_name_text(&name.to_utf16())
    }

    /// The descriptor of a @counter-style rule, serialized, or the empty string if the rule does not have it.
    pub fn counter_style_descriptor(&self, name: &str) -> Utf16String {
        self.descriptor_text(COUNTER_STYLE_AT_RULE, name)
            .unwrap_or_else(|| utf16(&[]))
    }

    /// https://drafts.csswg.org/css-counter-styles-3/#the-csscounterstylerule-interface
    /// Sets the descriptor to the value that the text parses to, and returns whether it did.
    pub fn set_counter_style_descriptor(&self, name: &str, text: &Utf16String) -> bool {
        // 1. parse the given value as the descriptor associated with the attribute.
        let name_units: Vec<u16> = name.encode_utf16().collect();
        let Some(value) = parse_descriptor_text(COUNTER_STYLE_AT_RULE, &name_units, &text.to_utf16()) else {
            // 2. If the result is invalid according to the given descriptor’s grammar, or would cause the
            //    @counter-style rule to not define a counter style, do nothing and abort these steps. (For example,
            //    some systems require the symbols descriptor to contain two values.)
            return false;
        };
        let rule = self.counter_style();
        match name {
            "symbols" | "additive-symbols" => {
                if !rule.system_accepts_symbols(name == "additive-symbols", &value) {
                    return false;
                }
            }
            // 3. If the attribute being set is system, and the new value would change the algorithm used, do nothing
            //    and abort these steps.
            // Note: It’s okay to change an aspect of the algorithm, like the first symbol value of a fixed system.
            "system" if rule.system_change_alters_algorithm(&value) => return false,
            _ => {}
        }

        // 4. Set the descriptor to the value.
        rule.set_descriptor(&name_units, value);
        true
    }

    pub fn font_feature_values_families(&self) -> Vec<Utf16String> {
        self.font_feature_values().families_text()
    }

    pub fn set_font_feature_values_families(&self, families: Vec<Utf16String>) {
        self.font_feature_values().set_families_text(families);
    }

    fn font_feature_values(&self) -> &crate::css::font_feature_values::FontFeatureValuesRule {
        let RulePayload::FontFeatureValues(values) = &self.payload else {
            unreachable!("expected a @font-feature-values rule")
        };
        values
    }

    pub fn set_font_feature_value(&self, kind: FontFeatureValuesRuleKind, name: &Utf16String, values: Vec<u32>) {
        self.font_feature_values().set_value(kind, &name.to_utf16(), values);
    }

    pub fn remove_font_feature_value(&self, kind: FontFeatureValuesRuleKind, name: &Utf16String) -> bool {
        self.font_feature_values().remove_value(kind, &name.to_utf16())
    }

    pub fn clear_font_feature_values(&self, kind: FontFeatureValuesRuleKind) {
        self.font_feature_values().clear_values(kind);
    }

    /// The name and the values of each feature value of the kind, in the order of the map.
    pub fn font_feature_values_entries(&self, kind: FontFeatureValuesRuleKind) -> Vec<(Utf16String, Vec<u32>)> {
        self.font_feature_values().entries_of(kind)
    }
}

impl NativeRuleList {
    /// The list that the pointer points to, which keeps its reference.
    ///
    /// # Safety
    ///
    /// `list` must point to a live list that an `Rc` owns.
    pub unsafe fn retained_from_raw(list: *const Self) -> Rc<Self> {
        // SAFETY: The caller passes a pointer into a live Rc.
        unsafe {
            Rc::increment_strong_count(list);
            Rc::from_raw(list)
        }
    }

    pub fn cssom_length(&self) -> usize {
        self.len()
    }

    pub fn cssom_rule_at(&self, index: usize) -> Rc<NativeRule> {
        self.rule_at(index)
    }

    pub fn cssom_identity_at(&self, index: usize) -> u64 {
        super::rust_rule_list_identity_at(self, index)
    }

    /// The path of indices from the list down to the rule with the identity.
    pub fn cssom_path_to_rule(&self, identity: u64) -> Option<Vec<usize>> {
        let mut path = Vec::new();
        self.find_path(identity, &mut path).then_some(path)
    }

    pub fn cssom_insert_rule(&self, index: usize, rule: Rc<NativeRule>, nested: bool) -> RuleInsertionResult {
        self.insert_css_rule(index, rule, nested)
    }

    pub fn cssom_append_rule(&self, rule: Rc<NativeRule>) {
        self.insert(self.len(), rule);
    }

    pub fn cssom_validate_removal(&self, index: usize) -> RuleRemovalResult {
        self.validate_removal(index)
    }

    pub fn cssom_remove(&self, index: usize) {
        self.remove(index);
    }

    pub fn cssom_replace(&self, source: &Self) {
        self.replace(source);
    }

    pub fn cssom_external_memory_size(&self) -> usize {
        super::rust_rule_list_external_memory_size(self)
    }
}
