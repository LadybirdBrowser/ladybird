/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::container_conditions::ContainerConditionsData;
use crate::css::counter_style::FfiCounterStyle;
use crate::css::css_string::CssString;
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::declaration_block::{DeclarationBlock, visit_declaration_images};
use crate::css::descriptor_block::FfiDescriptorBlock;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::font_feature_values::FontFeatureValuesRule;
use crate::css::function_signature::FunctionSignature;
use crate::css::import_rule::ImportRuleData;
use crate::css::keyframes::FfiKeyframe;
use crate::css::layer_names::LayerNames;
use crate::css::media_list::MediaList;
use crate::css::namespace_rule::NamespaceRuleData;
use crate::css::parser::query_parser::{FfiQueryHandle, MatchResult};
use crate::css::parser::syntax_parser::{PageSelectorList, ParsedRuleList, ParsedStyleSheet};
use crate::css::property_rule::PropertyRuleData;
use crate::css::scope_selectors::ScopeSelectors;
use crate::css::selector_operations::{
    absolutize_selector_list, adapt_scope_end_selector_list, scope_root_selector_list,
};
use crate::css::selector_parser::{RustBoundSelectorList, RustParsedSelectorList, StyleNestingParent};
use crate::css::style_rule::StyleRule;
use std::borrow::Cow;
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::rc::{Rc, Weak};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

mod compilation;
mod function;
pub(crate) mod mutation;
pub(crate) mod read;
use read::RuleRef;

// https://drafts.csswg.org/cssom/#dom-cssrule-type
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub enum NativeRuleType {
    Style = 1,
    Import = 3,
    Media = 4,
    FontFace = 5,
    Page = 6,
    Keyframes = 7,
    Keyframe = 8,
    Margin = 9,
    Namespace = 10,
    CounterStyle = 11,
    Supports = 12,
    FontFeatureValues = 14,
    // AD-HOC: These are internal types, not CSSOM type attribute values.
    LayerBlock = 100,
    LayerStatement = 101,
    NestedDeclarations = 102,
    Property = 103,
    Function = 104,
    FunctionDeclarations = 105,
    Container = 106,
    Scope = 107,
}

// Borrowed payload handles used when creating or inspecting a document-local
// rule. Only the fields belonging to the rule's type are populated.
#[derive(Default)]
#[repr(C)]
pub struct FfiRulePayload {
    pub has_source_position: bool,
    pub start_line: usize,
    pub start_column: usize,
    pub import_rule: *const ImportRuleData,
    pub media: *const MediaList,
    pub declarations: *const DeclarationBlock,
    pub descriptors: *const FfiDescriptorBlock,
    pub page_selectors: *const PageSelectorList,
    pub keyframe: *const FfiKeyframe,
    pub name: FfiUtf16View,
    pub namespace_rule: *const NamespaceRuleData,
    pub counter_style: *const FfiCounterStyle,
    pub supports: *const FfiQueryHandle,
    pub font_feature_values: *const FontFeatureValuesRule,
    pub layer_names: *const LayerNames,
    pub property: *const PropertyRuleData,
    pub function_signature: *const FunctionSignature,
    pub container: *const ContainerConditionsData,
    pub scope: *const ScopeSelectors,
}

pub(crate) enum RulePayload {
    Style(StyleRule),
    Import {
        data: Arc<ImportRuleData>,
        media: MediaList,
        scope: Option<Arc<ScopeSelectors>>,
    },
    Media {
        list: MediaList,
    },
    FontFace(Box<FfiDescriptorBlock>),
    Page {
        selectors: RefCell<Arc<PageSelectorList>>,
        descriptors: Box<FfiDescriptorBlock>,
    },
    Keyframes(RefCell<CssString>),
    Keyframe(Box<FfiKeyframe>),
    Margin {
        name: CssString,
        declarations: Box<DeclarationBlock>,
    },
    Namespace(Arc<NamespaceRuleData>),
    CounterStyle(Box<FfiCounterStyle>),
    Supports(Arc<FfiQueryHandle>),
    FontFeatureValues(Box<FontFeatureValuesRule>),
    LayerNames(Arc<LayerNames>),
    NestedDeclarations(Box<DeclarationBlock>),
    Property(Arc<PropertyRuleData>),
    Function(Arc<FunctionSignature>),
    FunctionDeclarations(Box<FfiDescriptorBlock>),
    Container(Arc<ContainerConditionsData>),
    Scope(Arc<ScopeSelectors>),
}

// Parsed data remains immutable and thread-shareable. These records are
// document-local owners of live payloads and child lists, without CSSOM pointers.
pub struct NativeRule {
    identity: u64,
    rule_type: NativeRuleType,
    pub(crate) payload: RulePayload,
    children: Option<Rc<NativeRuleList>>,
    source_position: Option<(usize, usize)>,
    parent: RefCell<Weak<NativeRule>>,
    matching_selectors: RefCell<Option<MatchingSelectorCache>>,
    scope_end_selectors: RefCell<Option<Rc<RustBoundSelectorList>>>,
}

#[cfg(test)]
thread_local! {
    pub(crate) static RULE_OWNER_ALLOCATIONS: Cell<u64> = const { Cell::new(0) };
}

struct MatchingSelectorCache {
    source: Arc<RustParsedSelectorList>,
    parent_kind: StyleNestingParent,
    parents: Option<Rc<RustBoundSelectorList>>,
    selectors: Rc<RustBoundSelectorList>,
}

pub struct NativeRuleList {
    rules: RefCell<Option<Vec<Rc<NativeRule>>>>,
    exposed_rules: RefCell<HashMap<usize, Rc<NativeRule>>>,
    identity_base: Cell<u64>,
    // Document media results, never rule or declaration owners in the shared parsed graph.
    media_results: Rc<RefCell<HashMap<u64, RuleMediaResults>>>,
    // Imported sheets and their CSSOM facade share this document's loading media state.
    import_media: Rc<RefCell<HashMap<u64, MediaList>>>,
    owner: RefCell<Weak<NativeRule>>,
    // Keep the immutable cached parse alive independently of CSSOM mutations.
    parsed_source: RefCell<Option<ParsedRuleList>>,
}

#[derive(Clone, Default)]
struct RuleMediaResults {
    matches: Vec<bool>,
}

impl NativeRule {
    pub(crate) fn declaration_block(&self) -> Option<&DeclarationBlock> {
        match &self.payload {
            RulePayload::Style(data) => Some(&data.declarations),
            RulePayload::NestedDeclarations(data) | RulePayload::Margin { declarations: data, .. } => Some(data),
            RulePayload::Keyframe(data) => Some(data.declaration_block()),
            _ => None,
        }
    }

    pub(crate) fn declaration_owner_identity(&self) -> Option<u64> {
        // Keyframe blocks contribute through their enclosing keyframes definition. Function
        // declaration blocks can be nested in conditional groups, but belong to the function.
        if self.rule_type == NativeRuleType::Keyframe {
            return self.parent.borrow().upgrade().map(|parent| parent.identity);
        }
        if self.rule_type == NativeRuleType::FunctionDeclarations {
            let mut parent = self.parent.borrow().upgrade();
            while let Some(rule) = parent {
                if rule.rule_type == NativeRuleType::Function {
                    return Some(rule.identity);
                }
                parent = rule.parent.borrow().upgrade();
            }
            return None;
        }
        Some(self.identity)
    }

    pub(crate) fn cascade_declarations(&self) -> Option<Arc<crate::css::declaration_block::DeclarationBlockData>> {
        match &self.payload {
            RulePayload::Style(style) => Some(style.declarations.data()),
            RulePayload::NestedDeclarations(declarations) => Some(declarations.data()),
            _ => None,
        }
    }

    pub(crate) fn style_rule(&self) -> &StyleRule {
        let RulePayload::Style(style) = &self.payload else {
            unreachable!("expected a style rule")
        };
        style
    }

    pub(crate) fn set_selector_text(&self, input: TokenizerInput<'_>, sheet_rules: Option<&NativeRuleList>) -> bool {
        let mut prefixes = Vec::new();
        if let Some(sheet_rules) = sheet_rules {
            sheet_rules.for_each_namespace(|namespace| prefixes.push(namespace.prefix.clone()));
        }
        let namespaces: Vec<_> = prefixes
            .iter()
            .map(|prefix| TokenizerInput::Utf16(prefix.units()))
            .collect();
        self.style_rule()
            .set_selector_text(input, &namespaces, self.nesting_parent_kind())
    }

    fn nesting_parent_kind(&self) -> StyleNestingParent {
        self.nesting_parent().map_or(StyleNestingParent::None, |parent| {
            if parent.rule_type == NativeRuleType::Style {
                StyleNestingParent::Style
            } else {
                StyleNestingParent::Scope
            }
        })
    }

    fn nesting_parent(&self) -> Option<Rc<NativeRule>> {
        let mut parent = self.parent.borrow().upgrade();
        while let Some(rule) = parent {
            if matches!(rule.rule_type, NativeRuleType::Style | NativeRuleType::Scope) {
                return Some(rule);
            }
            parent = rule.parent.borrow().upgrade();
        }
        None
    }

    pub(crate) unsafe fn matching_selectors(&self) -> Rc<RustBoundSelectorList> {
        let parent = self.nesting_parent();
        let parent_kind = parent.as_ref().map_or(StyleNestingParent::None, |parent| {
            if parent.rule_type == NativeRuleType::Style {
                StyleNestingParent::Style
            } else {
                StyleNestingParent::Scope
            }
        });
        let parents = if parent_kind == StyleNestingParent::Style {
            Some(unsafe { parent.as_ref().unwrap().matching_selectors() })
        } else {
            None
        };
        if self.rule_type == NativeRuleType::NestedDeclarations {
            if let Some(parents) = parents {
                return parents;
            }
            assert_eq!(parent_kind, StyleNestingParent::Scope);
            // https://drafts.csswg.org/css-cascade-6/#scoped-declarations
            // Declarations may be used directly with the body of a @scope rule. Contiguous runs of declarations are
            // wrapped in nested declarations rules, which match the scoping root with zero specificity.
            return Rc::new(RustBoundSelectorList {
                selectors: scope_root_selector_list(),
            });
        }
        let RulePayload::Style(style) = &self.payload else {
            unreachable!()
        };
        let source = style.selectors();
        unsafe { self.cached_matching_selectors(source, parent_kind, parents) }
    }

    unsafe fn cached_matching_selectors(
        &self,
        source: Arc<RustParsedSelectorList>,
        parent_kind: StyleNestingParent,
        parents: Option<Rc<RustBoundSelectorList>>,
    ) -> Rc<RustBoundSelectorList> {
        if let Some(cached) = self.matching_selectors.borrow().as_ref()
            && Arc::ptr_eq(&source, &cached.source)
            && parent_kind == cached.parent_kind
            && match (&parents, &cached.parents) {
                (Some(parents), Some(cached)) => Rc::ptr_eq(parents, cached),
                (None, None) => true,
                _ => false,
            }
        {
            return cached.selectors.clone();
        }
        let bound = unsafe { source.bind() };
        let empty: crate::css::selector::SelectorList = Box::new([]);
        let parent_selectors = parents.as_ref().map_or(&empty, |parents| &parents.selectors);
        let selectors = Rc::new(RustBoundSelectorList {
            selectors: absolutize_selector_list(&bound.selectors, parent_kind, parent_selectors)
                .unwrap_or(bound.selectors),
        });
        *self.matching_selectors.borrow_mut() = Some(MatchingSelectorCache {
            source,
            parent_kind,
            parents,
            selectors: selectors.clone(),
        });
        selectors
    }

    fn scope(&self) -> &ScopeSelectors {
        match &self.payload {
            RulePayload::Scope(scope) | RulePayload::Import { scope: Some(scope), .. } => scope,
            _ => unreachable!(),
        }
    }

    pub(crate) unsafe fn scope_start_selectors(&self) -> Option<Rc<RustBoundSelectorList>> {
        let source = self.scope().start.as_ref()?.clone();
        // Imports start a new sheet. A scope prelude uses its immediate enclosing rule's context.
        let parent = if self.rule_type == NativeRuleType::Scope {
            self.parent.borrow().upgrade()
        } else {
            None
        };
        let parent_kind = parent
            .as_ref()
            .map_or(StyleNestingParent::None, |parent| match parent.rule_type {
                NativeRuleType::Style => StyleNestingParent::Style,
                NativeRuleType::Scope => StyleNestingParent::Scope,
                _ => StyleNestingParent::None,
            });
        let parents = if parent_kind == StyleNestingParent::Style {
            Some(unsafe { parent.as_ref().unwrap().matching_selectors() })
        } else {
            None
        };
        Some(unsafe { self.cached_matching_selectors(source, parent_kind, parents) })
    }

    pub(crate) unsafe fn scope_end_selectors(&self) -> Option<Rc<RustBoundSelectorList>> {
        let source = self.scope().end.as_ref()?;
        if let Some(cached) = self.scope_end_selectors.borrow().as_ref() {
            return Some(cached.clone());
        }
        let bound = unsafe { source.bind() };
        let selectors = Rc::new(RustBoundSelectorList {
            selectors: adapt_scope_end_selector_list(&bound.selectors),
        });
        *self.scope_end_selectors.borrow_mut() = Some(selectors.clone());
        Some(selectors)
    }

    fn internal_layer_name(&self) -> Option<Cow<'_, [u16]>> {
        RuleRef::Materialized(self).internal_layer_name()
    }

    pub(crate) fn new(
        rule_type: NativeRuleType,
        payload: RulePayload,
        children: Option<Rc<NativeRuleList>>,
        source_position: Option<(usize, usize)>,
    ) -> Rc<Self> {
        Self::with_identity(
            reserve_rule_identities(1),
            rule_type,
            payload,
            children,
            source_position,
        )
    }

    pub(crate) fn with_identity(
        identity: u64,
        rule_type: NativeRuleType,
        payload: RulePayload,
        children: Option<Rc<NativeRuleList>>,
        source_position: Option<(usize, usize)>,
    ) -> Rc<Self> {
        #[cfg(test)]
        RULE_OWNER_ALLOCATIONS.with(|count| count.set(count.get().checked_add(1).unwrap()));
        let rule = Rc::new(Self {
            identity,
            rule_type,
            payload,
            children,
            source_position,
            parent: RefCell::new(Weak::new()),
            matching_selectors: RefCell::new(None),
            scope_end_selectors: RefCell::new(None),
        });
        if let Some(children) = &rule.children {
            *children.owner.borrow_mut() = Rc::downgrade(&rule);
            if let Some(materialized) = children.rules.borrow().as_ref() {
                for child in materialized {
                    *child.parent.borrow_mut() = Rc::downgrade(&rule);
                }
            }
        }
        rule
    }
}

impl NativeRuleList {
    pub(crate) fn replace(&self, source: &Self) {
        if std::ptr::eq(self, source) {
            return;
        }
        let rules = source.rules.borrow().clone();
        let exposed = source.exposed_rules.borrow().clone();
        let parsed_source = source.parsed_source.borrow().clone();
        let media_results = source.media_results.borrow().clone();
        let import_media = source.import_media.borrow().clone();
        self.clear();
        for rule in rules.iter().flatten().chain(exposed.values()) {
            *rule.parent.borrow_mut() = self.owner.borrow().clone();
        }
        self.rules.replace(rules);
        self.exposed_rules.replace(exposed);
        self.identity_base.set(source.identity_base.get());
        self.media_results.replace(media_results);
        self.import_media.replace(import_media);
        *self.parsed_source.borrow_mut() = parsed_source;
    }

    pub(crate) fn remove_imports(&self) {
        let has_imports = self
            .visit_rules(&mut |rule| {
                if rule.rule_type() == NativeRuleType::Import {
                    std::ops::ControlFlow::Break(())
                } else {
                    std::ops::ControlFlow::Continue(())
                }
            })
            .is_break();
        if !has_imports {
            return;
        }
        self.materialized_rules_mut().retain(|rule| {
            if rule.rule_type == NativeRuleType::Import {
                *rule.parent.borrow_mut() = Weak::new();
                false
            } else {
                true
            }
        });
    }

    pub(crate) fn len(&self) -> usize {
        if let Some(rules) = self.rules.borrow().as_ref() {
            return rules.len();
        }
        self.parsed_source.borrow().as_ref().unwrap().len()
    }

    pub(crate) fn insert(&self, index: usize, rule: Rc<NativeRule>) {
        *rule.parent.borrow_mut() = self.owner.borrow().clone();
        self.materialized_rules_mut().insert(index, rule);
    }

    pub(crate) fn insert_css_rule(&self, index: usize, rule: Rc<NativeRule>, nested: bool) -> RuleInsertionResult {
        let rule_type = rule.rule_type;
        {
            let rules = self.materialized_rules();
            if index > rules.len() {
                return RuleInsertionResult::IndexSizeError;
            }
            let before = &rules[..index];
            let after = &rules[index..];
            let import_after = after.iter().any(|rule| rule.rule_type == NativeRuleType::Import);
            let disallowed = match rule_type {
                NativeRuleType::LayerStatement => false,
                NativeRuleType::Import => before
                    .iter()
                    .any(|rule| !matches!(rule.rule_type, NativeRuleType::Import | NativeRuleType::LayerStatement)),
                NativeRuleType::Namespace => {
                    import_after
                        || before.iter().any(|rule| {
                            !matches!(
                                rule.rule_type,
                                NativeRuleType::Import | NativeRuleType::Namespace | NativeRuleType::LayerStatement
                            )
                        })
                }
                _ => import_after || after.iter().any(|rule| rule.rule_type == NativeRuleType::Namespace),
            };
            // If new rule cannot be inserted into list at the zero-indexed position index due to
            // constraints specified by CSS, then throw a HierarchyRequestError exception.
            if disallowed || (nested && matches!(rule_type, NativeRuleType::Import | NativeRuleType::Namespace)) {
                return RuleInsertionResult::HierarchyRequestError;
            }
            // If new rule is an @namespace at-rule, and list contains anything other than @import
            // at-rules, and @namespace at-rules, throw an InvalidStateError exception.
            if rule_type == NativeRuleType::Namespace
                && rules
                    .iter()
                    .any(|rule| !matches!(rule.rule_type, NativeRuleType::Import | NativeRuleType::Namespace))
            {
                return RuleInsertionResult::InvalidStateError;
            }
        }
        // Insert new rule into list at the zero-indexed position index.
        self.insert(index, rule);
        RuleInsertionResult::Success
    }

    pub(crate) fn remove(&self, index: usize) {
        let rule = self.materialized_rules_mut().remove(index);
        *rule.parent.borrow_mut() = Weak::new();
    }

    pub(crate) fn validate_removal(&self, index: usize) -> RuleRemovalResult {
        let rules = self.materialized_rules();
        // If index is greater than or equal to length, then throw an IndexSizeError exception.
        let Some(rule) = rules.get(index) else {
            return RuleRemovalResult::IndexSizeError;
        };
        // If old rule is an @namespace at-rule, and list contains anything other than @import
        // at-rules, and @namespace at-rules, throw an InvalidStateError exception.
        if rule.rule_type == NativeRuleType::Namespace
            && rules
                .iter()
                .any(|rule| !matches!(rule.rule_type, NativeRuleType::Import | NativeRuleType::Namespace))
        {
            return RuleRemovalResult::InvalidStateError;
        }
        RuleRemovalResult::Success
    }

    pub(crate) fn clear(&self) {
        if let Some(rules) = self.rules.replace(Some(Vec::new())) {
            for rule in rules {
                *rule.parent.borrow_mut() = Weak::new();
            }
        } else {
            for rule in self.exposed_rules.borrow_mut().drain().map(|(_, rule)| rule) {
                *rule.parent.borrow_mut() = Weak::new();
            }
        }
    }

    pub(crate) fn from_parsed(parsed: Arc<ParsedStyleSheet>) -> Rc<Self> {
        Rc::new(Self {
            identity_base: Cell::new(reserve_rule_identities(parsed.identity_count())),
            media_results: Rc::default(),
            import_media: Rc::default(),
            rules: RefCell::new(None),
            exposed_rules: RefCell::default(),
            owner: RefCell::new(Weak::new()),
            parsed_source: RefCell::new(Some(ParsedRuleList::root(parsed))),
        })
    }

    pub(crate) fn parsed_children(&self, parsed: ParsedRuleList) -> Rc<Self> {
        Rc::new(Self {
            identity_base: self.identity_base.clone(),
            media_results: self.media_results.clone(),
            import_media: self.import_media.clone(),
            rules: RefCell::new(None),
            exposed_rules: RefCell::default(),
            owner: RefCell::new(Weak::new()),
            parsed_source: RefCell::new(Some(parsed)),
        })
    }

    pub(crate) fn identity_for(&self, local_identity: u64) -> u64 {
        self.identity_base.get().checked_add(local_identity).unwrap()
    }

    pub(crate) fn import_media_for(&self, identity: u64) -> Option<MediaList> {
        self.import_media.borrow().get(&identity).cloned()
    }

    pub(crate) fn rule_at(&self, index: usize) -> Rc<NativeRule> {
        if let Some(rules) = self.rules.borrow().as_ref() {
            return rules[index].clone();
        }
        if let Some(rule) = self.exposed_rules.borrow().get(&index) {
            return rule.clone();
        }
        let rule = self
            .parsed_source
            .borrow()
            .as_ref()
            .unwrap()
            .materialize_at(index, self);
        *rule.parent.borrow_mut() = self.owner.borrow().clone();
        if let Some(result) = self.media_results.borrow_mut().remove(&rule.identity) {
            match &rule.payload {
                RulePayload::Media { list } => {
                    list.set_matches(&result.matches);
                }
                RulePayload::Import { media, .. } => media.set_matches(&result.matches),
                _ => unreachable!(),
            }
        }
        self.exposed_rules.borrow_mut().insert(index, rule.clone());
        rule
    }

    fn materialized_rules(&self) -> Ref<'_, Vec<Rc<NativeRule>>> {
        if self.rules.borrow().is_none() {
            let rules = (0..self.len()).map(|index| self.rule_at(index)).collect();
            self.exposed_rules.borrow_mut().clear();
            self.rules.replace(Some(rules));
        }
        Ref::map(self.rules.borrow(), |rules| rules.as_ref().unwrap())
    }

    fn materialized_rules_mut(&self) -> RefMut<'_, Vec<Rc<NativeRule>>> {
        drop(self.materialized_rules());
        RefMut::map(self.rules.borrow_mut(), |rules| rules.as_mut().unwrap())
    }

    // https://drafts.csswg.org/css-namespaces/#syntax
    // Any @namespace rules must follow all @charset and @import rules and precede all other
    // non-ignored at-rules and style rules in a style sheet.
    pub(crate) fn for_each_namespace(&self, mut visit: impl FnMut(&NamespaceRuleData)) {
        let mut namespaces = Vec::new();
        let _ = self.visit_rules(&mut |rule| {
            match rule.rule_type() {
                NativeRuleType::Import => {}
                NativeRuleType::Namespace => namespaces.push(rule.namespace().unwrap()),
                _ => return std::ops::ControlFlow::Break(()),
            }
            std::ops::ControlFlow::Continue(())
        });
        let mut prefixes = HashSet::new();
        // NB: Visit only the last declaration of each prefix, including the default namespace.
        for namespace in namespaces.iter().rev() {
            if prefixes.insert(namespace.prefix.units()) {
                visit(namespace);
            }
        }
    }

    pub(crate) fn new(rules: Vec<Rc<NativeRule>>, parsed_source: Option<Arc<ParsedStyleSheet>>) -> Rc<Self> {
        Rc::new(Self {
            rules: RefCell::new(Some(rules)),
            exposed_rules: RefCell::default(),
            identity_base: Cell::new(0),
            media_results: Rc::default(),
            import_media: Rc::default(),
            owner: RefCell::new(Weak::new()),
            parsed_source: RefCell::new(parsed_source.map(ParsedRuleList::root)),
        })
    }

    fn find_path(&self, identity: u64, path: &mut Vec<usize>) -> bool {
        fn find(rule: RuleRef<'_>, identity: u64, path: &mut Vec<usize>) -> std::ops::ControlFlow<()> {
            if rule.identity() == identity {
                return std::ops::ControlFlow::Break(());
            }
            let mut index = 0;
            rule.visit_children(&mut |child| {
                path.push(index);
                find(child, identity, path)?;
                path.pop();
                index += 1;
                std::ops::ControlFlow::Continue(())
            })
        }
        let mut index = 0;
        self.visit_rules(&mut |rule| {
            path.push(index);
            find(rule, identity, path)?;
            path.pop();
            index += 1;
            std::ops::ControlFlow::Continue(())
        })
        .is_break()
    }
}

/// Immutable parsing input that can outlive edits to the document-local rule graph.
pub struct NativeNamespaceContext {
    pub(crate) prefixes: Box<[crate::css::css_string::CssString]>,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_namespace_context(list: &NativeRuleList) -> *const NativeNamespaceContext {
    let mut prefixes = Vec::new();
    list.for_each_namespace(|namespace| prefixes.push(namespace.prefix.clone()));
    if prefixes.is_empty() {
        return std::ptr::null();
    }
    Arc::into_raw(Arc::new(NativeNamespaceContext {
        prefixes: prefixes.into(),
    }))
}

/// # Safety
/// Context must be null or an owned reference returned by this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_namespace_context_retain(
    context: *const NativeNamespaceContext,
) -> *const NativeNamespaceContext {
    if !context.is_null() {
        unsafe { Arc::increment_strong_count(context) };
    }
    context
}

/// # Safety
/// Context must be null or an owned reference returned by this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_namespace_context_release(context: *const NativeNamespaceContext) {
    if !context.is_null() {
        drop(unsafe { Arc::from_raw(context) });
    }
}

/// # Safety
/// The name must be readable for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_keyframes_set_name(rule: &NativeRule, name: FfiUtf16View) {
    let RulePayload::Keyframes(current) = &rule.payload else {
        unreachable!()
    };
    *current.borrow_mut() = CssString::from_utf16(&unsafe { name.to_utf16() }.unwrap());
}

/// # Safety
/// The selector must be readable for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_keyframes_find_rule(rule: &NativeRule, selector: FfiUtf16View) -> usize {
    let Some(selectors) =
        (unsafe { selector.units() }).and_then(crate::css::parser::syntax_parser::parse_keyframe_selector_list)
    else {
        return usize::MAX;
    };
    let matches = |frame: &NativeRule| {
        let RulePayload::Keyframe(frame) = &frame.payload else {
            unreachable!()
        };
        frame.keys() == selectors
    };
    let children = rule.children.as_ref().unwrap();
    if let Some(rules) = children.rules.borrow().as_ref() {
        return rules.iter().rposition(|frame| matches(frame)).unwrap_or(usize::MAX);
    }
    let parsed = children.parsed_source.borrow();
    let parsed = parsed.as_ref().unwrap();
    let exposed = children.exposed_rules.borrow();
    (0..parsed.len())
        .rev()
        .find(|index| match exposed.get(index) {
            Some(frame) => matches(frame),
            None => *parsed.rule_at(*index).keyframe().unwrap().keys == selectors,
        })
        .unwrap_or(usize::MAX)
}

static NEXT_RULE_IDENTITY: AtomicU64 = AtomicU64::new(1);

fn reserve_rule_identities(count: u64) -> u64 {
    NEXT_RULE_IDENTITY
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(count))
        .expect("rule identity overflow")
}

unsafe fn retain_arc<T>(data: *const T) -> Arc<T> {
    assert!(!data.is_null());
    unsafe {
        Arc::increment_strong_count(data);
        Arc::from_raw(data)
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_nested_declarations_create(declarations: &DeclarationBlock) -> *const NativeRule {
    Rc::into_raw(NativeRule::new(
        NativeRuleType::NestedDeclarations,
        RulePayload::NestedDeclarations(Box::new(declarations.clone())),
        None,
        None,
    ))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_retain(rule: *const NativeRule) -> *const NativeRule {
    unsafe { Rc::increment_strong_count(rule) };
    rule
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_release(rule: *const NativeRule) {
    if !rule.is_null() {
        drop(unsafe { Rc::from_raw(rule) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_identity(rule: &NativeRule) -> u64 {
    rule.identity
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_nesting_parent_kind(rule: &NativeRule) -> StyleNestingParent {
    rule.nesting_parent_kind()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_containing_function(rule: &NativeRule) -> *const NativeRule {
    if rule.rule_type == NativeRuleType::Function {
        return rule;
    }
    let mut parent = rule.parent.borrow().upgrade();
    while let Some(rule) = parent {
        if rule.rule_type == NativeRuleType::Function {
            return Rc::as_ptr(&rule);
        }
        parent = rule.parent.borrow().upgrade();
    }
    std::ptr::null()
}

/// Parse a CSSOM selector edit using native namespace and nesting context.
///
/// # Safety
/// Input and the optional sheet rule list must be live for this call on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_set_selector_text(
    rule: &NativeRule,
    input: FfiUtf16View,
    sheet_rules: *const NativeRuleList,
) -> bool {
    unsafe { input.units() }.is_some_and(|input| rule.set_selector_text(input, unsafe { sheet_rules.as_ref() }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_selectors(rule: &NativeRule) -> *const c_void {
    Arc::as_ptr(&rule.style_rule().selectors()).cast()
}

/// # Safety
/// Call only for style or nested-declaration rules on the document thread.
/// The returned list must be released with `rust_bound_selector_list_destroy` on that same thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_matching_selectors(rule: &NativeRule) -> *mut c_void {
    let selectors = unsafe { rule.matching_selectors() };
    Box::into_raw(Box::new(RustBoundSelectorList {
        selectors: selectors.selectors.clone(),
    }))
    .cast()
}

/// # Safety
/// Call for a scope rule or scoped import on the document thread. A non-null result
/// must be destroyed with `rust_bound_selector_list_destroy`; null means the start is omitted.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_scope_start_selectors(rule: &NativeRule) -> *mut c_void {
    unsafe { rule.scope_start_selectors() }
        .map(|selectors| {
            Box::into_raw(Box::new(RustBoundSelectorList {
                selectors: selectors.selectors.clone(),
            }))
            .cast()
        })
        .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// Call for a scope rule or scoped import on the document thread. A non-null result
/// must be destroyed with `rust_bound_selector_list_destroy`; null means the end is omitted.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_scope_end_selectors(rule: &NativeRule) -> *mut c_void {
    unsafe { rule.scope_end_selectors() }
        .map(|selectors| {
            Box::into_raw(Box::new(RustBoundSelectorList {
                selectors: selectors.selectors.clone(),
            }))
            .cast()
        })
        .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// `visit` must accept `context` and a UTF-16 slice borrowed for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_internal_layer_name(
    rule: &NativeRule,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *const u16, usize),
) -> bool {
    let Some(name) = rule.internal_layer_name() else {
        return false;
    };
    unsafe {
        visit(context, name.as_ptr(), name.len());
    }
    true
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiRuleTraversalOrder {
    Preorder,
    Postorder,
}

/// Visit attachment images in postorder, including styles in inactive groups.
/// Imports register images when their own sheets are attached. Other declaration-bearing rule
/// types load resources when their values are consumed, rather than preloading them here.
///
/// # Safety
/// The callback must not mutate the rule tree and must retain any values it keeps after returning.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_visit_images(
    list: &NativeRuleList,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *const c_void),
) {
    unsafe fn visit_rule(
        rule: RuleRef<'_>,
        context: *mut c_void,
        visit: unsafe extern "C" fn(*mut c_void, *const c_void),
    ) {
        let _ = rule.visit_children(&mut |child| {
            unsafe { visit_rule(child, context, visit) };
            std::ops::ControlFlow::Continue(())
        });
        if rule.rule_type() == NativeRuleType::Style {
            visit_declaration_images(&rule.cascade_declarations().unwrap(), &mut |value| unsafe {
                visit(context, std::ptr::from_ref(value).cast());
            });
        }
    }
    let _ = list.visit_rules(&mut |rule| {
        unsafe { visit_rule(rule, context, visit) };
        std::ops::ControlFlow::Continue(())
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_type(rule: &NativeRule) -> NativeRuleType {
    rule.rule_type
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_declarations(rule: &NativeRule) -> *mut DeclarationBlock {
    rule.declaration_block()
        .map_or(std::ptr::null_mut(), |block| Box::into_raw(Box::new(block.clone())))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_external_memory_size(rule: &NativeRule) -> usize {
    let payload_size = match &rule.payload {
        RulePayload::FontFace(_) | RulePayload::Page { .. } | RulePayload::FunctionDeclarations(_) => {
            size_of::<FfiDescriptorBlock>()
        }
        RulePayload::Keyframes(name) => {
            let mut size = size_of_val(name.borrow().units());
            let _ = rule.children.as_ref().unwrap().visit_rules(&mut |frame| {
                let (keys, declarations) = frame.keyframe();
                size = size
                    .saturating_add(size_of::<crate::css::keyframes::KeyframeData>())
                    .saturating_add(size_of_val(keys))
                    .saturating_add(declarations.external_memory_size());
                std::ops::ControlFlow::Continue(())
            });
            size
        }
        RulePayload::Keyframe(_) => size_of::<FfiKeyframe>(),
        RulePayload::Margin { name, .. } => size_of::<DeclarationBlock>().saturating_add(size_of_val(name.units())),
        RulePayload::CounterStyle(_) => size_of::<FfiCounterStyle>(),
        RulePayload::FontFeatureValues(_) => size_of::<FontFeatureValuesRule>(),
        RulePayload::NestedDeclarations(_) => size_of::<DeclarationBlock>(),
        _ => 0,
    };
    // Backing data is accounted for by the typed payload consumers.
    size_of::<NativeRule>().saturating_add(payload_size)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_payload(rule: &NativeRule) -> FfiRulePayload {
    let mut result = FfiRulePayload::default();
    if let Some((line, column)) = rule.source_position {
        result.has_source_position = true;
        result.start_line = line;
        result.start_column = column;
    }
    match &rule.payload {
        RulePayload::Style(_) => {}
        RulePayload::Import { data, media, scope } => {
            result.import_rule = Arc::as_ptr(data);
            result.media = std::ptr::from_ref(media);
            result.scope = scope.as_ref().map_or(std::ptr::null(), Arc::as_ptr);
        }
        RulePayload::Media { list, .. } => result.media = std::ptr::from_ref(list),
        RulePayload::FontFace(data) | RulePayload::FunctionDeclarations(data) => result.descriptors = &raw const **data,
        RulePayload::Page { selectors, descriptors } => {
            result.page_selectors = Arc::as_ptr(&selectors.borrow());
            result.descriptors = &raw const **descriptors;
        }
        RulePayload::Keyframes(name) => {
            let name = name.borrow();
            result.name = FfiUtf16View {
                utf16: name.units().as_ptr(),
                length: name.units().len(),
                ..Default::default()
            };
        }
        RulePayload::Keyframe(data) => result.keyframe = &raw const **data,
        RulePayload::Margin { name, declarations } => {
            result.name = FfiUtf16View {
                ascii: std::ptr::null(),
                utf16: name.units().as_ptr(),
                length: name.units().len(),
            };
            result.declarations = &raw const **declarations;
        }
        RulePayload::Namespace(data) => result.namespace_rule = Arc::as_ptr(data),
        RulePayload::CounterStyle(data) => result.counter_style = &raw const **data,
        RulePayload::Supports(data) => result.supports = Arc::as_ptr(data),
        RulePayload::FontFeatureValues(data) => result.font_feature_values = &raw const **data,
        RulePayload::LayerNames(data) => result.layer_names = Arc::as_ptr(data),
        RulePayload::NestedDeclarations(data) => result.declarations = &raw const **data,
        RulePayload::Property(data) => result.property = Arc::as_ptr(data),
        RulePayload::Function(data) => result.function_signature = Arc::as_ptr(data),
        RulePayload::Container(data) => result.container = Arc::as_ptr(data),
        RulePayload::Scope(data) => result.scope = Arc::as_ptr(data),
    }
    result
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_set_page_selectors(rule: &NativeRule, selectors: *const PageSelectorList) {
    let RulePayload::Page { selectors: current, .. } = &rule.payload else {
        unreachable!()
    };
    *current.borrow_mut() = unsafe { retain_arc(selectors) };
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_children(rule: &NativeRule) -> *const NativeRuleList {
    rule.children.as_ref().map_or(std::ptr::null(), Rc::as_ptr)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_create() -> *const NativeRuleList {
    Rc::into_raw(NativeRuleList::new(Vec::new(), None))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_replace(list: &NativeRuleList, source: &NativeRuleList) {
    list.replace(source);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_remove_imports(list: &NativeRuleList) {
    list.remove_imports();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_retain(list: *const NativeRuleList) -> *const NativeRuleList {
    unsafe { Rc::increment_strong_count(list) };
    list
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_release(list: *const NativeRuleList) {
    if !list.is_null() {
        drop(unsafe { Rc::from_raw(list) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_count(list: &NativeRuleList) -> usize {
    list.len()
}

/// Visit the effective namespace declarations without exposing rule wrappers.
///
/// # Safety
/// The callback must not mutate the list or retain the borrowed string views.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_visit_namespaces(
    list: &NativeRuleList,
    context: *const c_void,
    callback: unsafe extern "C" fn(*const c_void, FfiUtf16View, FfiUtf16View),
) {
    let view = |units: &[u16]| FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: units.as_ptr(),
        length: units.len(),
    };
    list.for_each_namespace(|namespace| unsafe {
        callback(context, view(namespace.prefix.units()), view(namespace.uri.units()));
    });
}

/// Resolve a live rule's current CSSOM path without creating wrappers for unrelated rules.
///
/// # Safety
/// `visit_path` must accept `context` and a borrowed index slice for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_find_path(
    list: &NativeRuleList,
    identity: u64,
    context: *mut c_void,
    visit_path: unsafe extern "C" fn(*mut c_void, *const usize, usize),
) {
    let mut path = Vec::new();
    if list.find_path(identity, &mut path) {
        unsafe { visit_path(context, path.as_ptr(), path.len()) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_at(list: &NativeRuleList, index: usize) -> *const NativeRule {
    Rc::as_ptr(&list.rule_at(index))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_identity_at(list: &NativeRuleList, index: usize) -> u64 {
    if let Some(rules) = list.rules.borrow().as_ref() {
        return rules[index].identity;
    }
    list.identity_for(
        list.parsed_source
            .borrow()
            .as_ref()
            .unwrap()
            .rule_at(index)
            .local_identity(),
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_insert(list: &NativeRuleList, index: usize, rule: *const NativeRule) {
    unsafe { Rc::increment_strong_count(rule) };
    list.insert(index, unsafe { Rc::from_raw(rule) });
}

#[repr(u8)]
#[derive(Debug, PartialEq, Eq)]
pub enum RuleInsertionResult {
    Success,
    IndexSizeError,
    HierarchyRequestError,
    InvalidStateError,
}

/// https://drafts.csswg.org/cssom/#insert-a-css-rule
///
/// # Safety
/// Rule must be a live Rc-owned native rule. Call on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_list_insert_css_rule(
    list: &NativeRuleList,
    index: usize,
    rule: *const NativeRule,
    nested: bool,
) -> RuleInsertionResult {
    unsafe { Rc::increment_strong_count(rule) };
    list.insert_css_rule(index, unsafe { Rc::from_raw(rule) }, nested)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_remove(list: &NativeRuleList, index: usize) {
    list.remove(index);
}

#[repr(u8)]
#[derive(Debug, PartialEq, Eq)]
pub enum RuleRemovalResult {
    Success,
    IndexSizeError,
    InvalidStateError,
}

/// https://drafts.csswg.org/cssom/#remove-a-css-rule
#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_validate_removal(list: &NativeRuleList, index: usize) -> RuleRemovalResult {
    list.validate_removal(index)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_clear(list: &NativeRuleList) {
    list.clear();
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_external_memory_size(list: &NativeRuleList) -> usize {
    let rules = list
        .rules
        .borrow()
        .as_ref()
        .map_or(0, |rules| rules.capacity().saturating_mul(size_of::<Rc<NativeRule>>()));
    let exposed = list
        .exposed_rules
        .borrow()
        .capacity()
        .saturating_mul(size_of::<(usize, Rc<NativeRule>)>());
    let results = list.media_results.borrow();
    let media = results.values().fold(
        results.capacity().saturating_mul(size_of::<(u64, RuleMediaResults)>()),
        |size, result| size.saturating_add(result.matches.capacity()),
    );
    let imports = list
        .import_media
        .borrow()
        .capacity()
        .saturating_mul(size_of::<(u64, MediaList)>());
    size_of::<NativeRuleList>()
        .saturating_add(rules)
        .saturating_add(exposed)
        .saturating_add(media)
        .saturating_add(imports)
}
