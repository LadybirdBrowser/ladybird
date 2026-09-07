/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{NativeRule, NativeRuleList, NativeRuleType, RulePayload};
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::css_string::CssString;
use crate::css::declaration_block::DeclarationBlockData;
use crate::css::descriptor_block::{DescriptorBlockData, FfiDescriptorBlock};
use crate::css::font_feature_values::FontFeatureValuesData;
use crate::css::function_signature::FunctionSignature;
use crate::css::import_rule::ImportRuleData;
use crate::css::layer_names::LayerNames;
use crate::css::media_list::MediaList;
use crate::css::namespace_rule::NamespaceRuleData;
use crate::css::parser::query_parser::MediaEnvironment;
use crate::css::parser::syntax_parser::SharedRule;
use crate::css::property_rule::PropertyRuleData;
use crate::css::scope_selectors::ScopeSelectors;
use crate::css::selector_parser::RustParsedSelectorList;
use std::borrow::Cow;
use std::ops::ControlFlow;
use std::sync::Arc;

#[derive(Clone, Copy)]
pub(crate) enum RuleRef<'a> {
    Materialized(&'a NativeRule),
    Shared(SharedRule<'a>, &'a NativeRuleList),
}

// Borrowed for one host callback. Retaining immutable payload data never retains this view
// or creates a live CSSOM rule. The host must not keep the view past the callback.
pub struct NativeRuleView<'a> {
    pub(crate) rule: RuleRef<'a>,
    pub(crate) containers: &'a [Arc<ContainerConditionsData>],
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_change_needs_style_environment_bump(rule: &NativeRule) -> bool {
    RuleRef::Materialized(rule).change_needs_style_environment_bump()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_identity(view: &NativeRuleView<'_>) -> u64 {
    view.rule.identity()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_type(view: &NativeRuleView<'_>) -> NativeRuleType {
    view.rule.rule_type()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_property(view: &NativeRuleView<'_>) -> *const PropertyRuleData {
    match view.rule {
        RuleRef::Materialized(rule) => match &rule.payload {
            RulePayload::Property(property) => Arc::as_ptr(property),
            _ => std::ptr::null(),
        },
        RuleRef::Shared(rule, _) => rule.property().map_or(std::ptr::null(), std::ptr::from_ref),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_container(view: &NativeRuleView<'_>) -> *const ContainerConditionsData {
    view.rule.container().map_or(std::ptr::null(), Arc::as_ptr)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_descriptors(view: &NativeRuleView<'_>) -> *mut FfiDescriptorBlock {
    Box::into_raw(Box::new(FfiDescriptorBlock::new(view.rule.descriptors().unwrap())))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_import(view: &NativeRuleView<'_>) -> *const ImportRuleData {
    match view.rule {
        RuleRef::Shared(rule, _) => Arc::as_ptr(rule.import().unwrap()),
        RuleRef::Materialized(rule) => {
            let RulePayload::Import { data, .. } = &rule.payload else {
                unreachable!()
            };
            Arc::as_ptr(data)
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_import_media(view: &NativeRuleView<'_>) -> *mut MediaList {
    let media = match view.rule {
        RuleRef::Shared(rule, list) => list
            .import_media
            .borrow_mut()
            .entry(view.rule.identity())
            .or_insert_with(|| MediaList::new(rule.media_data()))
            .clone(),
        RuleRef::Materialized(rule) => {
            let RulePayload::Import { media, .. } = &rule.payload else {
                unreachable!()
            };
            media.clone()
        }
    };
    Box::into_raw(Box::new(media))
}

/// Materialize a facade's import rule, including one first exposed after detachment.
///
/// # Safety
/// Identity must identify this import. Data must be Arc-owned; media must be its loading state.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_materialize_import(
    identity: u64,
    data: *const ImportRuleData,
    media: &MediaList,
) -> *const NativeRule {
    unsafe {
        Arc::increment_strong_count(data);
    }
    let data = unsafe { Arc::from_raw(data) };
    let scope = data.scope.clone();
    std::rc::Rc::into_raw(NativeRule::with_identity(
        identity,
        NativeRuleType::Import,
        RulePayload::Import {
            data,
            media: media.clone(),
            scope,
        },
        None,
        None,
    ))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_font_feature_values(view: &NativeRuleView<'_>) -> *const FontFeatureValuesData {
    let data = match view.rule {
        RuleRef::Shared(rule, _) => rule.font_feature_values().unwrap(),
        RuleRef::Materialized(rule) => {
            let RulePayload::FontFeatureValues(values) = &rule.payload else {
                unreachable!()
            };
            values.snapshot()
        }
    };
    Arc::into_raw(data)
}

/// # Safety
/// The callback must not mutate the graph or retain borrowed name text.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_view_name(
    view: &NativeRuleView<'_>,
    context: *mut std::ffi::c_void,
    visit: unsafe extern "C" fn(*mut std::ffi::c_void, *const u16, usize),
) {
    let name = view.rule.definition_name().unwrap();
    unsafe {
        visit(context, name.units().as_ptr(), name.units().len());
    }
}

/// # Safety
/// The callback must not mutate the graph. Keys and declarations are borrowed for the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_view_keyframes(
    view: &NativeRuleView<'_>,
    context: *const std::ffi::c_void,
    visit: unsafe extern "C" fn(*const std::ffi::c_void, *const f64, usize, *const DeclarationBlockData),
) {
    assert!(view.rule.rule_type() == NativeRuleType::Keyframes);
    let _ = view.rule.visit_children(&mut |frame| {
        let (keys, declarations) = frame.keyframe();
        unsafe {
            visit(context, keys.as_ptr(), keys.len(), Arc::as_ptr(&declarations));
        }
        ControlFlow::Continue(())
    });
}

impl<'a> RuleRef<'a> {
    pub(crate) fn change_needs_style_environment_bump(self) -> bool {
        match self.rule_type() {
            NativeRuleType::Style
            | NativeRuleType::Media
            | NativeRuleType::Supports
            | NativeRuleType::Container
            | NativeRuleType::Scope
            | NativeRuleType::LayerBlock => self
                .visit_children(&mut |child| {
                    if child.change_needs_style_environment_bump() {
                        ControlFlow::Break(())
                    } else {
                        ControlFlow::Continue(())
                    }
                })
                .is_break(),
            NativeRuleType::NestedDeclarations
            | NativeRuleType::Property
            | NativeRuleType::LayerStatement
            | NativeRuleType::Keyframes
            | NativeRuleType::FontFace
            | NativeRuleType::Function
            | NativeRuleType::CounterStyle
            | NativeRuleType::FontFeatureValues => false,
            _ => true,
        }
    }

    pub(crate) fn function_signature(self) -> Option<Arc<FunctionSignature>> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Function(signature) => Some(signature.clone()),
                _ => None,
            },
            Self::Shared(rule, _) => rule.function_signature(),
        }
    }

    pub(crate) fn descriptors(self) -> Option<Arc<DescriptorBlockData>> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::FontFace(block)
                | RulePayload::FunctionDeclarations(block)
                | RulePayload::Page { descriptors: block, .. } => Some(block.data()),
                RulePayload::CounterStyle(rule) => Some(rule.descriptors()),
                _ => None,
            },
            Self::Shared(rule, _) => rule.descriptors(),
        }
    }

    pub(super) fn keyframe(self) -> (&'a [f64], Arc<DeclarationBlockData>) {
        match self {
            Self::Materialized(rule) => {
                let RulePayload::Keyframe(frame) = &rule.payload else {
                    unreachable!()
                };
                (frame.keys(), frame.declarations())
            }
            Self::Shared(rule, _) => {
                let frame = rule.keyframe().unwrap();
                (&frame.keys, frame.declarations.clone())
            }
        }
    }
    pub(crate) fn rule_type(self) -> NativeRuleType {
        match self {
            Self::Materialized(rule) => rule.rule_type,
            Self::Shared(rule, _) => rule.rule_type(),
        }
    }

    pub(crate) fn identity(self) -> u64 {
        match self {
            Self::Materialized(rule) => rule.identity,
            Self::Shared(rule, list) => list.identity_for(rule.local_identity()),
        }
    }

    pub(crate) fn internal_layer_name(self) -> Option<Cow<'a, [u16]>> {
        let name = match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::LayerNames(names) if rule.rule_type == NativeRuleType::LayerBlock => {
                    names.names[0].units()
                }
                RulePayload::Import { data, .. } => data.layer.as_ref()?.units(),
                _ => return None,
            },
            Self::Shared(rule, _) => rule.layer_name()?,
        };
        if !name.is_empty() {
            return Some(Cow::Borrowed(name));
        }
        let mut buffer = [0_u16; 21];
        let mut start = buffer.len();
        let mut identity = self.identity();
        loop {
            start -= 1;
            buffer[start] = u16::from(b'0') + (identity % 10) as u16;
            identity /= 10;
            if identity == 0 {
                break;
            }
        }
        start -= 1;
        buffer[start] = u16::from(b'#');
        Some(Cow::Owned(buffer[start..].to_vec()))
    }

    pub(crate) fn definition_name(self) -> Option<CssString> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Property(property) => Some(property.name.clone()),
                RulePayload::Keyframes(name) => Some(name.borrow().clone()),
                RulePayload::CounterStyle(rule) => Some(rule.name()),
                _ => None,
            },
            Self::Shared(rule, _) => rule.definition_name(),
        }
    }

    pub(crate) fn layer_names(self) -> Option<&'a LayerNames> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::LayerNames(names) => Some(names),
                _ => None,
            },
            Self::Shared(rule, _) => rule.layer_names(),
        }
    }

    pub(crate) fn container(self) -> Option<&'a Arc<ContainerConditionsData>> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Container(container) => Some(container),
                _ => None,
            },
            Self::Shared(rule, _) => rule.container(),
        }
    }

    pub(crate) fn condition_holds(self, environment: MediaEnvironment<'_>) -> bool {
        if !self.supports_hold() {
            return false;
        }
        if self.rule_type() == NativeRuleType::Media {
            self.evaluate_media(environment)
        } else if self.rule_type() == NativeRuleType::Import {
            self.media_matches()
        } else {
            true
        }
    }

    pub(crate) fn supports_hold(self) -> bool {
        let condition = match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Supports(condition) => Some(condition),
                RulePayload::Import { data, .. } => data.supports.as_ref(),
                _ => None,
            },
            Self::Shared(rule, _) => rule.supports(),
        };
        condition.is_none_or(|condition| condition.evaluate_supports() == Some(super::MatchResult::True))
    }

    pub(crate) fn cached_condition_holds(self) -> bool {
        self.supports_hold()
            && (!matches!(self.rule_type(), NativeRuleType::Media | NativeRuleType::Import) || self.media_matches())
    }

    pub(crate) fn media_matches(self) -> bool {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Media { list, .. } | RulePayload::Import { media: list, .. } => list.matches(),
                _ => unreachable!(),
            },
            Self::Shared(rule, list) => {
                if rule.rule_type() == NativeRuleType::Import
                    && let Some(media) = list.import_media.borrow().get(&self.identity())
                {
                    return media.matches();
                }
                let results = list.media_results.borrow();
                results.get(&self.identity()).map_or_else(
                    || rule.media().unwrap().queries.is_empty(),
                    |result| result.matches.is_empty() || result.matches.iter().any(|matches| *matches),
                )
            }
        }
    }

    pub(crate) fn evaluate_media(self, environment: MediaEnvironment<'_>) -> bool {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Media { list, .. } | RulePayload::Import { media: list, .. } => list.evaluate(environment),
                _ => unreachable!(),
            },
            Self::Shared(rule, list) => {
                let mut results = list.media_results.borrow_mut();
                let result = results.entry(self.identity()).or_default();
                result.matches.clear();
                result.matches.extend(
                    rule.media()
                        .unwrap()
                        .queries
                        .iter()
                        .map(|query| query.matches_media(environment)),
                );
                result.matches.is_empty() || result.matches.iter().any(|matches| *matches)
            }
        }
    }

    pub(crate) fn namespace(self) -> Option<Arc<NamespaceRuleData>> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Namespace(namespace) => Some(namespace.clone()),
                _ => None,
            },
            Self::Shared(rule, _) => rule.namespace().cloned(),
        }
    }

    pub(crate) fn selectors(self) -> Option<Arc<RustParsedSelectorList>> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Style(style) => Some(style.selectors()),
                _ => None,
            },
            Self::Shared(rule, _) => rule.selectors(),
        }
    }

    pub(crate) fn scope(self) -> Option<Arc<ScopeSelectors>> {
        match self {
            Self::Materialized(rule) => match &rule.payload {
                RulePayload::Scope(scope) | RulePayload::Import { scope: Some(scope), .. } => Some(scope.clone()),
                _ => None,
            },
            Self::Shared(rule, _) => rule.scope(),
        }
    }

    pub(crate) fn cascade_declarations(self) -> Option<Arc<DeclarationBlockData>> {
        match self {
            Self::Materialized(rule) => rule.cascade_declarations(),
            Self::Shared(rule, _) => rule.cascade_declarations(),
        }
    }

    pub(crate) fn visit_children(self, visit: &mut impl FnMut(RuleRef<'_>) -> ControlFlow<()>) -> ControlFlow<()> {
        match self {
            Self::Materialized(rule) => match &rule.children {
                Some(children) => children.visit_rules(visit),
                None => ControlFlow::Continue(()),
            },
            Self::Shared(rule, base) => rule.visit_children(&mut |child| visit(Self::Shared(child, base))),
        }
    }
}

impl NativeRuleList {
    pub(crate) fn visit_descendant_rules(
        &self,
        visit: &mut impl FnMut(RuleRef<'_>) -> ControlFlow<()>,
    ) -> ControlFlow<()> {
        fn walk(rule: RuleRef<'_>, visit: &mut impl FnMut(RuleRef<'_>) -> ControlFlow<()>) -> ControlFlow<()> {
            visit(rule)?;
            rule.visit_children(&mut |child| walk(child, visit))
        }
        self.visit_rules(&mut |rule| walk(rule, visit))
    }

    pub(crate) fn visit_rules(&self, visit: &mut impl FnMut(RuleRef<'_>) -> ControlFlow<()>) -> ControlFlow<()> {
        if let Some(rules) = self.rules.borrow().as_ref() {
            for rule in rules {
                visit(RuleRef::Materialized(rule))?;
            }
            return ControlFlow::Continue(());
        }
        let parsed = self.parsed_source.borrow();
        let exposed = self.exposed_rules.borrow();
        let mut index = 0;
        parsed.as_ref().unwrap().visit_rules(&mut |rule| {
            let rule = match exposed.get(&index) {
                Some(rule) => RuleRef::Materialized(rule),
                None => RuleRef::Shared(rule, self),
            };
            index += 1;
            visit(rule)
        })
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_descriptor_snapshot(list: &NativeRuleList, identity: u64) -> *mut FfiDescriptorBlock {
    let mut result = std::ptr::null_mut();
    let _ = list.visit_descendant_rules(&mut |rule| {
        if rule.identity() != identity {
            return ControlFlow::Continue(());
        }
        result = Box::into_raw(Box::new(FfiDescriptorBlock::new(rule.descriptors().unwrap())));
        ControlFlow::Break(())
    });
    result
}

/// # Safety
/// The callback must not mutate the list or retain the borrowed view.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_visit_font_rules(
    list: &NativeRuleList,
    context: *const std::ffi::c_void,
    visit: unsafe extern "C" fn(*const std::ffi::c_void, &NativeRuleView<'_>),
) {
    let _ = list.visit_descendant_rules(&mut |rule| {
        if matches!(
            rule.rule_type(),
            NativeRuleType::FontFace | NativeRuleType::FontFeatureValues
        ) {
            unsafe {
                visit(context, &NativeRuleView { rule, containers: &[] });
            }
        }
        ControlFlow::Continue(())
    });
}

/// # Safety
/// The callback must not mutate the rule list or retain the borrowed view.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_visit_rule_data(
    list: &NativeRuleList,
    context: *const std::ffi::c_void,
    visit: unsafe extern "C" fn(*const std::ffi::c_void, &NativeRuleView<'_>) -> bool,
) {
    let _ = list.visit_rules(&mut |rule| {
        if unsafe { visit(context, &NativeRuleView { rule, containers: &[] }) } {
            ControlFlow::Continue(())
        } else {
            ControlFlow::Break(())
        }
    });
}
