/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    NativeRuleType, NestedDeclarationsKind, ParsedDeclarationList, ParsedRule, ParsedRuleChild, ParsedRuleKind,
    ParsedStyleSheet,
};
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::css_string::CssString;
use crate::css::declaration_block::DeclarationBlockData;
use crate::css::descriptor_block::DescriptorBlockData;
use crate::css::font_feature_values::FontFeatureValuesData;
use crate::css::function_signature::FunctionSignature;
use crate::css::import_rule::ImportRuleData;
use crate::css::keyframes::KeyframeData;
use crate::css::layer_names::LayerNames;
use crate::css::media_list::MediaListData;
use crate::css::namespace_rule::NamespaceRuleData;
use crate::css::parser::query_parser::FfiQueryHandle;
use crate::css::property_rule::PropertyRuleData;
use crate::css::scope_selectors::ScopeSelectors;
use crate::css::selector_parser::RustParsedSelectorList;
use std::ops::ControlFlow;
use std::sync::Arc;

pub(super) fn native_child_indices(children: &[ParsedRuleChild], parent: Option<ParsedRuleKind>) -> Box<[usize]> {
    if !matches!(
        parent,
        None | Some(
            ParsedRuleKind::Qualified
                | ParsedRuleKind::Media
                | ParsedRuleKind::Supports
                | ParsedRuleKind::Container
                | ParsedRuleKind::Scope
                | ParsedRuleKind::Layer
                | ParsedRuleKind::Function
                | ParsedRuleKind::Page
                | ParsedRuleKind::Keyframes
        )
    ) {
        return Box::default();
    }
    children
        .iter()
        .enumerate()
        .filter_map(|(index, child)| {
            let kind = match child {
                ParsedRuleChild::Rule(rule) => rule.native_type()?,
                ParsedRuleChild::Declarations(_) => NativeRuleType::NestedDeclarations,
            };
            if (parent == Some(ParsedRuleKind::Page) && kind != NativeRuleType::Margin)
                || (parent == Some(ParsedRuleKind::Keyframes) && kind != NativeRuleType::Keyframe)
            {
                return None;
            }
            Some(index)
        })
        .collect()
}

// An immutable list projection retains the original graph, not copies of its rules.
#[derive(Clone)]
pub(crate) struct ParsedRuleList {
    sheet: Arc<ParsedStyleSheet>,
    parent: Option<(Arc<ParsedRule>, NestedDeclarationsKind)>,
}

impl ParsedRuleList {
    pub(crate) fn root(sheet: Arc<ParsedStyleSheet>) -> Self {
        Self { sheet, parent: None }
    }

    pub(super) fn children(
        sheet: Arc<ParsedStyleSheet>,
        parent: Arc<ParsedRule>,
        declarations_kind: NestedDeclarationsKind,
    ) -> Self {
        Self {
            sheet,
            parent: Some((parent, declarations_kind)),
        }
    }

    pub(crate) fn visit_rules<'a>(
        &'a self,
        visit: &mut impl FnMut(SharedRule<'a>) -> ControlFlow<()>,
    ) -> ControlFlow<()> {
        for index in 0..self.len() {
            visit(self.rule_at(index))?;
        }
        ControlFlow::Continue(())
    }

    pub(crate) fn len(&self) -> usize {
        self.parent
            .as_ref()
            .map_or(self.sheet.native_roots.len(), |(parent, _)| {
                parent.native_children.len()
            })
    }

    pub(crate) fn rule_at(&self, index: usize) -> SharedRule<'_> {
        match &self.parent {
            Some((parent, kind)) => {
                SharedRule::from_child(&parent.children[parent.native_children[index]], *kind).unwrap()
            }
            None => {
                let index = self.sheet.native_roots[index];
                if self.sheet.items.is_empty() {
                    SharedRule::from_rule(&self.sheet.rules[index], self.sheet.declarations_kind).unwrap()
                } else {
                    SharedRule::from_child(&self.sheet.items[index], self.sheet.declarations_kind).unwrap()
                }
            }
        }
    }

    pub(crate) fn materialize_at(
        &self,
        index: usize,
        list: &crate::css::rule::NativeRuleList,
    ) -> std::rc::Rc<crate::css::rule::NativeRule> {
        let rule = self.rule_at(index);
        match rule.data {
            SharedRuleData::Rule(parsed) => self.sheet.native_rule(parsed, rule.declarations_kind, list).unwrap(),
            SharedRuleData::Declarations(declarations) => {
                self.sheet
                    .native_declarations(declarations, rule.declarations_kind, list)
            }
        }
    }
}

#[derive(Clone, Copy)]
enum SharedRuleData<'a> {
    Rule(&'a Arc<ParsedRule>),
    Declarations(&'a ParsedDeclarationList),
}

// A borrowed view of the worker's graph, not a document-local rule owner.
#[derive(Clone, Copy)]
pub(crate) struct SharedRule<'a> {
    data: SharedRuleData<'a>,
    declarations_kind: NestedDeclarationsKind,
    rule_type: NativeRuleType,
}

impl NativeRuleType {
    pub(super) fn child_declarations_kind(self, enclosing: NestedDeclarationsKind) -> Option<NestedDeclarationsKind> {
        match self {
            NativeRuleType::Style => Some(NestedDeclarationsKind::Style),
            NativeRuleType::Function => Some(NestedDeclarationsKind::Function),
            NativeRuleType::Media
            | NativeRuleType::Supports
            | NativeRuleType::Container
            | NativeRuleType::Scope
            | NativeRuleType::LayerBlock
            | NativeRuleType::Page
            | NativeRuleType::Keyframes => Some(enclosing),
            _ => None,
        }
    }
}

impl ParsedRule {
    pub(super) fn native_type(&self) -> Option<NativeRuleType> {
        Some(match self.rule_kind {
            ParsedRuleKind::Invalid
            | ParsedRuleKind::Unknown
            | ParsedRuleKind::IgnoredVendor
            | ParsedRuleKind::FontFeatureValuesRule => return None,
            ParsedRuleKind::Qualified if self.keyframe.is_some() => NativeRuleType::Keyframe,
            ParsedRuleKind::Qualified => NativeRuleType::Style,
            ParsedRuleKind::Import => NativeRuleType::Import,
            ParsedRuleKind::Media => NativeRuleType::Media,
            ParsedRuleKind::Supports => NativeRuleType::Supports,
            ParsedRuleKind::Container => NativeRuleType::Container,
            ParsedRuleKind::Scope => NativeRuleType::Scope,
            ParsedRuleKind::Layer if self.has_block => NativeRuleType::LayerBlock,
            ParsedRuleKind::Layer => NativeRuleType::LayerStatement,
            ParsedRuleKind::Function => NativeRuleType::Function,
            ParsedRuleKind::Page => NativeRuleType::Page,
            ParsedRuleKind::Margin => NativeRuleType::Margin,
            ParsedRuleKind::FontFace => NativeRuleType::FontFace,
            ParsedRuleKind::FontFeatureValues => NativeRuleType::FontFeatureValues,
            ParsedRuleKind::Keyframes => NativeRuleType::Keyframes,
            ParsedRuleKind::Namespace => NativeRuleType::Namespace,
            ParsedRuleKind::CounterStyle => NativeRuleType::CounterStyle,
            ParsedRuleKind::Property => NativeRuleType::Property,
        })
    }
}

impl<'a> SharedRule<'a> {
    fn from_rule(rule: &'a Arc<ParsedRule>, declarations_kind: NestedDeclarationsKind) -> Option<Self> {
        Some(Self {
            data: SharedRuleData::Rule(rule),
            declarations_kind,
            rule_type: rule.native_type()?,
        })
    }

    fn from_child(child: &'a ParsedRuleChild, declarations_kind: NestedDeclarationsKind) -> Option<Self> {
        match child {
            ParsedRuleChild::Rule(rule) => Self::from_rule(rule, declarations_kind),
            ParsedRuleChild::Declarations(declarations) => Some(Self {
                data: SharedRuleData::Declarations(declarations),
                declarations_kind,
                rule_type: match declarations_kind {
                    NestedDeclarationsKind::Style => NativeRuleType::NestedDeclarations,
                    NestedDeclarationsKind::Function => NativeRuleType::FunctionDeclarations,
                },
            }),
        }
    }

    pub(crate) fn rule_type(self) -> NativeRuleType {
        self.rule_type
    }

    pub(crate) fn local_identity(self) -> u64 {
        match self.data {
            SharedRuleData::Rule(rule) => rule.local_identity,
            SharedRuleData::Declarations(declarations) => declarations.local_identity,
        }
    }

    pub(crate) fn namespace(self) -> Option<&'a Arc<NamespaceRuleData>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.namespace_rule.as_ref()
    }

    pub(crate) fn property(self) -> Option<&'a PropertyRuleData> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.property_rule.as_deref()
    }

    pub(crate) fn import(self) -> Option<&'a Arc<ImportRuleData>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.import_rule.as_ref()
    }

    pub(crate) fn media_data(self) -> Arc<MediaListData> {
        let SharedRuleData::Rule(rule) = self.data else {
            unreachable!()
        };
        rule.media_list.as_ref().unwrap().clone()
    }

    pub(crate) fn function_signature(self) -> Option<Arc<FunctionSignature>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.function_signature.clone()
    }

    pub(crate) fn font_feature_values(self) -> Option<Arc<FontFeatureValuesData>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.font_feature_values.clone()
    }

    pub(crate) fn descriptors(self) -> Option<Arc<DescriptorBlockData>> {
        match self.data {
            SharedRuleData::Rule(rule) => rule.descriptor_block.clone(),
            SharedRuleData::Declarations(declarations) => declarations.descriptor_block.clone(),
        }
    }

    pub(crate) fn keyframe(self) -> Option<&'a KeyframeData> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.keyframe.as_deref()
    }

    pub(crate) fn selectors(self) -> Option<Arc<RustParsedSelectorList>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.selector_list.clone()
    }

    pub(crate) fn scope(self) -> Option<Arc<ScopeSelectors>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.scope_selectors.clone()
    }

    pub(crate) fn container(self) -> Option<&'a Arc<ContainerConditionsData>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.container_conditions.as_ref()
    }

    pub(crate) fn layer_name(self) -> Option<&'a [u16]> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        match self.rule_type {
            NativeRuleType::LayerBlock => Some(rule.layer_names.as_ref().unwrap().names[0].units()),
            NativeRuleType::Import => Some(rule.import_rule.as_ref().unwrap().layer.as_ref()?.units()),
            _ => None,
        }
    }

    pub(crate) fn definition_name(self) -> Option<CssString> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        match self.rule_type {
            NativeRuleType::Property => Some(rule.property_rule.as_ref().unwrap().name.clone()),
            NativeRuleType::Keyframes => rule.keyframes_name.clone(),
            NativeRuleType::CounterStyle => Some(rule.counter_style.as_ref().unwrap().name.clone()),
            _ => None,
        }
    }

    pub(crate) fn layer_names(self) -> Option<&'a LayerNames> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.layer_names.as_deref()
    }

    pub(crate) fn media(self) -> Option<&'a MediaListData> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        rule.media_list.as_deref()
    }

    pub(crate) fn supports(self) -> Option<&'a Arc<FfiQueryHandle>> {
        let SharedRuleData::Rule(rule) = self.data else {
            return None;
        };
        if self.rule_type == NativeRuleType::Import {
            rule.import_rule.as_ref().unwrap().supports.as_ref()
        } else {
            rule.supports_condition.as_ref()
        }
    }

    pub(crate) fn cascade_declarations(self) -> Option<Arc<DeclarationBlockData>> {
        match self.data {
            SharedRuleData::Rule(rule) if self.rule_type == NativeRuleType::Style => rule.declaration_block.clone(),
            SharedRuleData::Declarations(declarations) if self.rule_type == NativeRuleType::NestedDeclarations => {
                declarations.declaration_block.clone()
            }
            _ => None,
        }
    }

    pub(crate) fn visit_children(self, visit: &mut impl FnMut(Self) -> ControlFlow<()>) -> ControlFlow<()> {
        let SharedRuleData::Rule(rule) = self.data else {
            return ControlFlow::Continue(());
        };
        let Some(declarations_kind) = self.rule_type.child_declarations_kind(self.declarations_kind) else {
            return ControlFlow::Continue(());
        };
        for &index in &rule.native_children {
            let child = Self::from_child(&rule.children[index], declarations_kind).unwrap();
            visit(child)?;
        }
        ControlFlow::Continue(())
    }
}
