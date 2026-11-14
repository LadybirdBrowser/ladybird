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
use crate::css::declaration_block::SharedDeclarationBlock;
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

/// A position among a sheet's rules or a rule's children, which fits in 32 bits.
pub(super) fn narrow_index(index: usize) -> u32 {
    u32::try_from(index).expect("style sheet has too many rules")
}

/// The positions among `children` of the rules the CSSOM sees, where the rules of `children` are indices among `rules`.
pub(super) fn native_child_indices<'a>(
    rules: &'a [ParsedRule],
    children: &'a [ParsedRuleChild],
    parent: Option<ParsedRuleKind>,
) -> impl Iterator<Item = u32> + 'a {
    let has_native_children = matches!(
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
    );
    let children = if has_native_children { children } else { &[] };
    children.iter().enumerate().filter_map(move |(index, child)| {
        let kind = match child {
            &ParsedRuleChild::Rule(rule) => rules[rule as usize].native_type()?,
            ParsedRuleChild::Declarations(_) => NativeRuleType::NestedDeclarations,
        };
        if (parent == Some(ParsedRuleKind::Page) && kind != NativeRuleType::Margin)
            || (parent == Some(ParsedRuleKind::Keyframes) && kind != NativeRuleType::Keyframe)
        {
            return None;
        }
        Some(narrow_index(index))
    })
}

// An immutable list projection retains the original graph, not copies of its rules.
#[derive(Clone)]
pub(crate) struct ParsedRuleList {
    sheet: Arc<ParsedStyleSheet>,
    /// The index among the rules of the sheet of the rule whose children the list is, or none for the top-level
    /// rules of the sheet.
    parent: Option<(u32, NestedDeclarationsKind)>,
}

impl ParsedRuleList {
    pub(crate) fn root_sheet(&self) -> Option<&Arc<ParsedStyleSheet>> {
        self.parent.is_none().then_some(&self.sheet)
    }

    pub(crate) fn root(sheet: Arc<ParsedStyleSheet>) -> Self {
        Self { sheet, parent: None }
    }

    pub(super) fn children(
        sheet: Arc<ParsedStyleSheet>,
        parent: u32,
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

    /// Visits the rules of [`Self::visit_rules`] that are, or hold, a rule a media evaluation reads: of a sheet's
    /// top-level rules, the few its parse found; of a rule's children, all.
    pub(crate) fn visit_media_holding_rules<'a>(
        &'a self,
        visit: &mut impl FnMut(SharedRule<'a>) -> ControlFlow<()>,
    ) -> ControlFlow<()> {
        if self.parent.is_some() {
            return self.visit_rules(visit);
        }
        for &position in &self.sheet.media_holding_roots {
            visit(self.rule_at(position as usize))?;
        }
        ControlFlow::Continue(())
    }

    pub(crate) fn len(&self) -> usize {
        self.parent.map_or(self.sheet.native_roots.len(), |(parent, _)| {
            self.sheet.native_children_of(self.sheet.rule(parent)).len()
        })
    }

    pub(crate) fn rule_at(&self, index: usize) -> SharedRule<'_> {
        let sheet = &*self.sheet;
        match self.parent {
            Some((parent, kind)) => {
                let parent = sheet.rule(parent);
                let position = sheet.native_children_of(parent)[index] as usize;
                SharedRule::from_child(sheet, &sheet.children_of(parent)[position], kind).unwrap()
            }
            None => {
                let index = sheet.native_roots[index] as usize;
                if sheet.items.is_empty() {
                    SharedRule::from_rule(sheet, sheet.roots[index], sheet.declarations_kind).unwrap()
                } else {
                    SharedRule::from_child(sheet, &sheet.items[index], sheet.declarations_kind).unwrap()
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
            SharedRuleData::Rule(_, rule_index) => self
                .sheet
                .native_rule(rule_index, rule.declarations_kind, list)
                .unwrap(),
            SharedRuleData::Declarations(declarations) => {
                self.sheet
                    .native_declarations(declarations, rule.declarations_kind, list)
            }
        }
    }
}

#[derive(Clone, Copy)]
enum SharedRuleData<'a> {
    /// A rule of the sheet, by its index among the rules of the sheet.
    Rule(&'a ParsedStyleSheet, u32),
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
            ParsedRuleKind::Qualified if self.keyframe().is_some() => NativeRuleType::Keyframe,
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
            ParsedRuleKind::PositionTry => NativeRuleType::PositionTry,
            ParsedRuleKind::FontFace => NativeRuleType::FontFace,
            ParsedRuleKind::FontFeatureValues => NativeRuleType::FontFeatureValues,
            ParsedRuleKind::Keyframes => NativeRuleType::Keyframes,
            ParsedRuleKind::Namespace => NativeRuleType::Namespace,
            ParsedRuleKind::CounterStyle => NativeRuleType::CounterStyle,
            ParsedRuleKind::Property => NativeRuleType::Property,
        })
    }
}

impl ParsedRule {
    /// Whether the rule is, or holds, a rule a media evaluation reads: a `@media` rule, or an `@import`, whose sheet
    /// may hold one.
    pub(super) fn holds_media_rules(&self, sheet: &ParsedStyleSheet) -> bool {
        matches!(self.rule_kind, ParsedRuleKind::Media | ParsedRuleKind::Import)
            || sheet
                .children_of(self)
                .iter()
                .any(|child| matches!(*child, ParsedRuleChild::Rule(rule) if sheet.rule(rule).holds_media_rules(sheet)))
    }
}

impl<'a> SharedRule<'a> {
    fn from_rule(sheet: &'a ParsedStyleSheet, index: u32, declarations_kind: NestedDeclarationsKind) -> Option<Self> {
        Some(Self {
            data: SharedRuleData::Rule(sheet, index),
            declarations_kind,
            rule_type: sheet.rule(index).native_type()?,
        })
    }

    fn from_child(
        sheet: &'a ParsedStyleSheet,
        child: &'a ParsedRuleChild,
        declarations_kind: NestedDeclarationsKind,
    ) -> Option<Self> {
        match child {
            &ParsedRuleChild::Rule(index) => Self::from_rule(sheet, index, declarations_kind),
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

    /// The parsed rule this is, unless it is a list of declarations.
    fn parsed(self) -> Option<&'a ParsedRule> {
        match self.data {
            SharedRuleData::Rule(sheet, index) => Some(sheet.rule(index)),
            SharedRuleData::Declarations(_) => None,
        }
    }

    pub(crate) fn rule_type(self) -> NativeRuleType {
        self.rule_type
    }

    pub(crate) fn local_identity(self) -> u64 {
        match self.data {
            SharedRuleData::Rule(sheet, index) => u64::from(sheet.rule(index).local_identity),
            SharedRuleData::Declarations(declarations) => declarations.local_identity,
        }
    }

    pub(crate) fn namespace(self) -> Option<&'a Arc<NamespaceRuleData>> {
        let rule = self.parsed()?;
        rule.namespace_rule()
    }

    pub(crate) fn property(self) -> Option<&'a PropertyRuleData> {
        let rule = self.parsed()?;
        rule.property_rule().map(|value| &**value)
    }

    pub(crate) fn import(self) -> Option<&'a Arc<ImportRuleData>> {
        let rule = self.parsed()?;
        rule.import_rule()
    }

    pub(crate) fn media_data(self) -> Arc<MediaListData> {
        let Some(rule) = self.parsed() else { unreachable!() };
        rule.media_list().unwrap().clone()
    }

    pub(crate) fn function_signature(self) -> Option<Arc<FunctionSignature>> {
        let rule = self.parsed()?;
        rule.function_signature().cloned()
    }

    pub(crate) fn font_feature_values(self) -> Option<Arc<FontFeatureValuesData>> {
        let rule = self.parsed()?;
        rule.font_feature_values().cloned()
    }

    pub(crate) fn descriptors(self) -> Option<Arc<DescriptorBlockData>> {
        match self.data {
            SharedRuleData::Rule(sheet, index) => sheet.rule(index).descriptor_block().cloned(),
            SharedRuleData::Declarations(declarations) => declarations.descriptor_block.clone(),
        }
    }

    pub(crate) fn keyframe(self) -> Option<&'a KeyframeData> {
        let rule = self.parsed()?;
        rule.keyframe().map(|value| &**value)
    }

    pub(crate) fn selectors(self) -> Option<RustParsedSelectorList> {
        let rule = self.parsed()?;
        rule.selector_list().cloned()
    }

    pub(crate) fn scope(self) -> Option<Arc<ScopeSelectors>> {
        let rule = self.parsed()?;
        rule.scope_selectors().cloned()
    }

    pub(crate) fn container(self) -> Option<&'a Arc<ContainerConditionsData>> {
        let rule = self.parsed()?;
        rule.container_conditions()
    }

    pub(crate) fn layer_name(self) -> Option<&'a [u16]> {
        let rule = self.parsed()?;
        match self.rule_type {
            NativeRuleType::LayerBlock => Some(rule.layer_names().unwrap().names[0].units()),
            NativeRuleType::Import => Some(rule.import_rule().unwrap().layer.as_ref()?.units()),
            _ => None,
        }
    }

    pub(crate) fn definition_name(self) -> Option<CssString> {
        let rule = self.parsed()?;
        match self.rule_type {
            NativeRuleType::Property => Some(rule.property_rule().unwrap().name.clone()),
            NativeRuleType::Keyframes => rule.keyframes_name().cloned(),
            NativeRuleType::PositionTry => rule.position_try_name().cloned(),
            NativeRuleType::CounterStyle => Some(rule.counter_style().unwrap().name.clone()),
            _ => None,
        }
    }

    pub(crate) fn layer_names(self) -> Option<&'a LayerNames> {
        let rule = self.parsed()?;
        rule.layer_names().map(|value| &**value)
    }

    pub(crate) fn media(self) -> Option<&'a MediaListData> {
        let rule = self.parsed()?;
        rule.media_list().map(|value| &**value)
    }

    pub(crate) fn supports(self) -> Option<&'a Arc<FfiQueryHandle>> {
        let rule = self.parsed()?;
        if self.rule_type == NativeRuleType::Import {
            rule.import_rule().unwrap().supports.as_ref()
        } else {
            rule.supports_condition()
        }
    }

    pub(crate) fn cascade_declarations(self) -> Option<SharedDeclarationBlock> {
        match self.data {
            SharedRuleData::Rule(sheet, index) if self.rule_type == NativeRuleType::Style => {
                sheet.rule(index).declaration_block().cloned()
            }
            SharedRuleData::Declarations(declarations) if self.rule_type == NativeRuleType::NestedDeclarations => {
                declarations.declaration_block.clone()
            }
            _ => None,
        }
    }

    pub(crate) fn position_try_declarations(self) -> Option<SharedDeclarationBlock> {
        match self.data {
            SharedRuleData::Rule(sheet, index) if self.rule_type == NativeRuleType::PositionTry => {
                sheet.rule(index).declaration_block().cloned()
            }
            _ => None,
        }
    }

    pub(crate) fn visit_children(self, visit: &mut impl FnMut(Self) -> ControlFlow<()>) -> ControlFlow<()> {
        let SharedRuleData::Rule(sheet, index) = self.data else {
            return ControlFlow::Continue(());
        };
        let Some(declarations_kind) = self.rule_type.child_declarations_kind(self.declarations_kind) else {
            return ControlFlow::Continue(());
        };
        let rule = sheet.rule(index);
        let children = sheet.children_of(rule);
        for &position in sheet.native_children_of(rule) {
            let child = Self::from_child(sheet, &children[position as usize], declarations_kind).unwrap();
            visit(child)?;
        }
        ControlFlow::Continue(())
    }
}
