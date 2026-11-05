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
use crate::css::ffi_support::{ArcRef, FfiUtf16View, OwnedArc, OwnedRc, RcRef};
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
use crate::css::selector::{SelectorListRef, SelectorListView};
use crate::css::selector_arena::{Nesting, NestingContext};
use crate::css::selector_operations::{
    absolutize_selector_list, adapt_scope_end_selector_list, scope_root_selector_list,
};
use crate::css::selector_parser::{RustParsedSelectorList, StyleNestingParent};
use crate::css::style_rule::StyleRule;
use std::borrow::Cow;
use std::cell::{Cell, OnceCell, Ref, RefCell, RefMut};
use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::rc::{Rc, Weak};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

mod compilation;
pub mod cssom;
mod function;
pub(crate) use function::CompiledFunction;
#[cfg(test)]
pub(crate) use function::compile_functions_for_testing;
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
    PositionTry = 108,
}

// Borrowed payload handles used when creating or inspecting a document-local
// rule. Only the fields belonging to the rule's type are populated.
#[derive(Default)]
#[repr(C)]
pub struct FfiRulePayload<'a> {
    pub has_source_position: bool,
    pub start_line: usize,
    pub start_column: usize,
    pub import_rule: *const ImportRuleData,
    pub media: *const MediaList,
    pub declarations: *const DeclarationBlock,
    pub descriptors: *const FfiDescriptorBlock,
    pub page_selectors: *const PageSelectorList,
    pub keyframe: *const FfiKeyframe,
    pub name: FfiUtf16View<'a>,
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
    PositionTry {
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
    /// The scope's selectors, which binding the rule's `&` to what encloses it replaces.
    Scope(RefCell<Arc<ScopeSelectors>>),
}

// Parsed data remains immutable and thread-shareable. These records are
// document-local owners of live payloads and child lists, without CSSOM pointers.
pub struct NativeRule {
    identity: u64,
    rule_type: NativeRuleType,
    pub(crate) payload: RulePayload,
    /// The rule's child rules: set as it is made for a group rule, and made empty when first asked
    /// for, for a style rule without nested rules, as most are.
    children: OnceCell<Rc<NativeRuleList>>,
    /// The line and column the rule starts at, when it was parsed from text.
    source_position: Option<(u32, u32)>,
    parent: Cell<Weak<NativeRule>>,
}

/// `list` with each `&` replaced by what it names, and a selector relative to a scoping root
/// prefixed with it, or `list` itself where nothing is.
fn absolutized(list: &SelectorListRef) -> SelectorListRef {
    fn tree(list: SelectorListView<'_>) -> Option<crate::css::selector_tree::SelectorList> {
        let (kind, parents) = match list.nesting() {
            Some(Nesting::Rule(parent)) => (
                StyleNestingParent::Style,
                tree(parent).unwrap_or_else(|| parent.to_tree()),
            ),
            Some(Nesting::Scope) => (StyleNestingParent::Scope, Default::default()),
            // NB: A `&` outside any style rule or `@scope` names the scoping root.
            Some(Nesting::Root) => (StyleNestingParent::None, Default::default()),
            // NB: Only the selectors of a rule directly in `@scope` start with a combinator.
            _ if list.iter().any(|selector| {
                selector.compound_selectors().first().is_some_and(|first| {
                    !matches!(
                        first.combinator,
                        crate::css::selector::Combinator::None | crate::css::selector::Combinator::Descendant
                    )
                })
            }) =>
            {
                (StyleNestingParent::Scope, Default::default())
            }
            _ => return None,
        };
        absolutize_selector_list(&list.to_tree(), kind, &parents)
    }
    // NB: The trees name the atoms of the arenas they came from, which are live while it is packed.
    tree(list.view()).map_or_else(|| list.clone(), |tree| SelectorListRef::pack(&tree))
}

#[cfg(test)]
thread_local! {
    pub(crate) static RULE_OWNER_ALLOCATIONS: Cell<u64> = const { Cell::new(0) };
}

pub struct NativeRuleList {
    rules: RefCell<Option<Vec<Rc<NativeRule>>>>,
    exposed_rules: RefCell<ExposedRules>,
    identity_base: Cell<u64>,
    // Document media results, never rule or declaration owners in the shared parsed graph.
    media_results: Rc<RefCell<HashMap<u64, RuleMediaResults>>>,
    // Imported sheets and their CSSOM facade share this document's loading media state.
    import_media: Rc<RefCell<HashMap<u64, MediaList>>>,
    owner: RefCell<Weak<NativeRule>>,
    // Keep the immutable cached parse alive independently of CSSOM mutations.
    parsed_source: RefCell<Option<ParsedRuleList>>,
}

/// The rules of a list the CSSOM has materialized one by one, by their index, before it
/// materializes the whole list. Boxed, as most lists, those of the style rules among them, never
/// expose one.
#[derive(Clone, Default)]
#[allow(clippy::box_collection, reason = "a box is one word where a map is six")]
struct ExposedRules(Option<Box<HashMap<usize, Rc<NativeRule>>>>);

impl ExposedRules {
    fn get(&self, index: &usize) -> Option<&Rc<NativeRule>> {
        self.0.as_ref()?.get(index)
    }

    fn insert(&mut self, index: usize, rule: Rc<NativeRule>) {
        self.0.get_or_insert_default().insert(index, rule);
    }

    fn is_empty(&self) -> bool {
        self.0.as_ref().is_none_or(|rules| rules.is_empty())
    }

    #[cfg(test)]
    fn len(&self) -> usize {
        self.0.as_ref().map_or(0, |rules| rules.len())
    }

    fn capacity(&self) -> usize {
        self.0.as_ref().map_or(0, |rules| rules.capacity())
    }

    fn clear(&mut self) {
        self.0 = None;
    }

    fn values(&self) -> impl Iterator<Item = &Rc<NativeRule>> {
        self.0.iter().flat_map(|rules| rules.values())
    }

    fn drain(&mut self) -> impl Iterator<Item = Rc<NativeRule>> {
        self.0.take().into_iter().flat_map(|rules| rules.into_values())
    }
}

#[derive(Clone, Default)]
struct RuleMediaResults {
    matches: Vec<bool>,
}

impl NativeRule {
    pub(crate) fn declaration_block(&self) -> Option<&DeclarationBlock> {
        match &self.payload {
            RulePayload::Style(data) => Some(&data.declarations),
            RulePayload::NestedDeclarations(data)
            | RulePayload::Margin { declarations: data, .. }
            | RulePayload::PositionTry { declarations: data, .. } => Some(data),
            RulePayload::Keyframe(data) => Some(data.declaration_block()),
            _ => None,
        }
    }

    pub(crate) fn declaration_owner_identity(&self) -> Option<u64> {
        // Keyframe blocks contribute through their enclosing keyframes definition. Function
        // declaration blocks can be nested in conditional groups, but belong to the function.
        if self.rule_type == NativeRuleType::Keyframe {
            return self.parent().map(|parent| parent.identity);
        }
        if self.rule_type == NativeRuleType::FunctionDeclarations {
            let mut parent = self.parent();
            while let Some(rule) = parent {
                if rule.rule_type == NativeRuleType::Function {
                    return Some(rule.identity);
                }
                parent = rule.parent();
            }
            return None;
        }
        Some(self.identity)
    }

    pub(crate) fn cascade_declarations(&self) -> Option<crate::css::declaration_block::SharedDeclarationBlock> {
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
        let mut declared = Vec::new();
        if let Some(sheet_rules) = sheet_rules {
            sheet_rules
                .for_each_namespace(|namespace| declared.push((namespace.prefix.clone(), namespace.uri.clone())));
        }
        let namespaces: Vec<_> = declared
            .iter()
            .map(|(prefix, _)| TokenizerInput::Utf16(prefix.units()))
            .collect();
        let declarations = crate::css::selector_arena::NamespaceDeclarations::of_texts(
            declared.iter().map(|(prefix, uri)| (prefix.units(), uri.units())),
        );
        if !self
            .style_rule()
            .set_selector_text(input, &namespaces, declarations, self.nesting_parent_kind())
        {
            return false;
        }
        // NB: The new selectors name the parent with `&`, and the nested rules' name these.
        self.bind_nesting();
        true
    }

    /// Binds each `&` in the selectors of the rule, and of the rules nested in it, to what encloses
    /// it, as placing the rule or changing what encloses it does. Selectors already naming that
    /// stay as they are.
    pub(crate) fn bind_nesting(&self) {
        let parent = self.parent();
        let parent_selectors = parent
            .as_ref()
            .filter(|parent| parent.rule_type == NativeRuleType::Style)
            .map(|parent| parent.style_rule().selectors());
        let parent = match (&parent, &parent_selectors) {
            (_, Some(selectors)) => NestingContext::Rule(&selectors.selectors),
            (Some(parent), None) if parent.rule_type == NativeRuleType::Scope => NestingContext::Scope,
            _ => NestingContext::Root,
        };
        let nesting = self.nesting_parent();
        let nesting_selectors = nesting
            .as_ref()
            .filter(|rule| rule.rule_type == NativeRuleType::Style)
            .map(|rule| rule.style_rule().selectors());
        let context = match (&nesting, &nesting_selectors) {
            (_, Some(selectors)) => NestingContext::Rule(&selectors.selectors),
            (Some(_), None) => NestingContext::Scope,
            (None, _) => NestingContext::Root,
        };
        self.bind_nesting_in(context, parent);
    }

    /// Binds as [`Self::bind_nesting`] does, where a `&` names `context`, and one in a
    /// `<scope-start>` names `parent`, what the rule is directly in.
    fn bind_nesting_in(&self, context: NestingContext<'_>, parent: NestingContext<'_>) {
        match &self.payload {
            RulePayload::Style(style) => {
                let selectors = style.selectors();
                if !selectors.selectors.is_nested_in(context) {
                    style.replace_selectors(Arc::new(RustParsedSelectorList {
                        selectors: selectors.selectors.nested_in(context),
                    }));
                }
                let selectors = style.selectors();
                let own = NestingContext::Rule(&selectors.selectors);
                self.bind_children(own, own);
            }
            RulePayload::Scope(scope) => {
                let current = scope.borrow().clone();
                if let Some(start) = &current.start
                    && !start.selectors.is_nested_in(parent)
                {
                    *scope.borrow_mut() = Arc::new(ScopeSelectors {
                        start: Some(Arc::new(RustParsedSelectorList {
                            selectors: start.selectors.nested_in(parent),
                        })),
                        end: current.end.clone(),
                    });
                }
                self.bind_children(NestingContext::Scope, NestingContext::Scope);
            }
            _ => self.bind_children(context, NestingContext::Root),
        }
    }

    fn bind_children(&self, context: NestingContext<'_>, parent: NestingContext<'_>) {
        let Some(children) = self.children.get() else {
            return;
        };
        for index in 0..children.len() {
            children.rule_at(index).bind_nesting_in(context, parent);
        }
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
        let mut parent = self.parent();
        while let Some(rule) = parent {
            if matches!(rule.rule_type, NativeRuleType::Style | NativeRuleType::Scope) {
                return Some(rule);
            }
            parent = rule.parent();
        }
        None
    }

    /// The rule that holds this one, while it is held.
    pub(crate) fn parent(&self) -> Option<Rc<NativeRule>> {
        let parent = self.parent.take();
        let upgraded = parent.upgrade();
        self.parent.set(parent);
        upgraded
    }

    pub(crate) fn set_parent(&self, parent: Weak<NativeRule>) {
        self.parent.set(parent);
    }

    /// The rule's child rules, for a grouping rule.
    pub(crate) fn child_rules(&self) -> Option<&Rc<NativeRuleList>> {
        if self.rule_type != NativeRuleType::Style {
            return self.children.get();
        }
        Some(self.children.get_or_init(|| {
            let children = NativeRuleList::new(Vec::new(), None);
            // SAFETY: Every rule lives in the Rc `with_identity` made it in.
            let this = unsafe {
                Rc::increment_strong_count(std::ptr::from_ref(self));
                Rc::from_raw(std::ptr::from_ref(self))
            };
            *children.owner.borrow_mut() = Rc::downgrade(&this);
            children
        }))
    }

    /// The selectors the rule matches by with each `&` replaced by what it names, as DevTools and
    /// rule dumps show them. Nested declarations match by their parent's.
    pub(crate) fn absolutized_selectors(&self) -> SelectorListRef {
        if self.rule_type == NativeRuleType::NestedDeclarations {
            let parent = self
                .nesting_parent()
                .expect("nested declarations are inside a style rule or `@scope`");
            if parent.rule_type == NativeRuleType::Style {
                return parent.absolutized_selectors();
            }
            // https://drafts.csswg.org/css-cascade-6/#scoped-declarations
            // Declarations may be used directly with the body of a @scope rule. Contiguous runs of declarations are
            // wrapped in nested declarations rules, which match the scoping root with zero specificity.
            return SelectorListRef::pack(&scope_root_selector_list());
        }
        absolutized(&self.style_rule().selectors().selectors)
    }

    fn scope(&self) -> Arc<ScopeSelectors> {
        match &self.payload {
            RulePayload::Scope(scope) => scope.borrow().clone(),
            RulePayload::Import { scope: Some(scope), .. } => scope.clone(),
            _ => unreachable!(),
        }
    }

    /// The `<scope-start>` of a scope rule or scoped import, absolutized as
    /// [`Self::absolutized_selectors`] are.
    pub(crate) fn absolutized_scope_start(&self) -> Option<SelectorListRef> {
        Some(absolutized(&self.scope().start.as_ref()?.selectors))
    }

    /// The `<scope-end>` of a scope rule or scoped import, relative to its scoping root.
    pub(crate) fn absolutized_scope_end(&self) -> Option<SelectorListRef> {
        let scope = self.scope();
        let end = scope.end.as_ref()?;
        // NB: The tree names the atoms of the end's arena, which is live while it is packed again.
        Some(SelectorListRef::pack(&adapt_scope_end_selector_list(
            &end.selectors.view().to_tree(),
        )))
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
            children: children.map_or_else(OnceCell::new, OnceCell::from),
            source_position: source_position.map(|(line, column)| {
                let narrow = |value: usize| u32::try_from(value).expect("style sheet text exceeds u32");
                (narrow(line), narrow(column))
            }),
            parent: Cell::new(Weak::new()),
        });
        if let Some(children) = rule.children.get() {
            *children.owner.borrow_mut() = Rc::downgrade(&rule);
            if let Some(materialized) = children.rules.borrow().as_ref() {
                for child in materialized {
                    child.set_parent(Rc::downgrade(&rule));
                }
            }
        }
        rule
    }
}

impl NativeRuleList {
    fn shared_contents(&self) -> Option<Arc<ParsedStyleSheet>> {
        if self.rules.borrow().is_some() || !self.exposed_rules.borrow().is_empty() {
            return None;
        }
        self.parsed_source.borrow().as_ref()?.root_sheet().cloned()
    }

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
            rule.set_parent(self.owner.borrow().clone());
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
                rule.set_parent(Weak::new());
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
        rule.set_parent(self.owner.borrow().clone());
        // NB: A rule parsed for a place inside a style rule or `@scope` names it with `&` once
        //     placed there.
        if rule.nesting_parent().is_some() {
            rule.bind_nesting();
        }
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
        let was_nested = rule.nesting_parent().is_some();
        rule.set_parent(Weak::new());
        // NB: A rule taken out of a style rule or `@scope` names neither with `&` any more.
        if was_nested {
            rule.bind_nesting();
        }
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
                rule.set_parent(Weak::new());
            }
        } else {
            for rule in self.exposed_rules.borrow_mut().drain() {
                rule.set_parent(Weak::new());
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
        rule.set_parent(self.owner.borrow().clone());
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
    /// The namespace each prefix names, in the order of `prefixes`.
    pub(crate) uris: Box<[crate::css::css_string::CssString]>,
}

impl NativeNamespaceContext {
    /// The declarations, as the selectors of a sheet carry them.
    pub(crate) fn declarations(&self) -> crate::css::selector_arena::NamespaceDeclarations {
        crate::css::selector_arena::NamespaceDeclarations::of_texts(
            self.prefixes
                .iter()
                .zip(&self.uris)
                .map(|(prefix, uri)| (prefix.units(), uri.units())),
        )
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_namespace_context(list: &NativeRuleList) -> *const NativeNamespaceContext {
    let (mut prefixes, mut uris) = (Vec::new(), Vec::new());
    list.for_each_namespace(|namespace| {
        prefixes.push(namespace.prefix.clone());
        uris.push(namespace.uri.clone());
    });
    if prefixes.is_empty() {
        return std::ptr::null();
    }
    Arc::into_raw(Arc::new(NativeNamespaceContext {
        prefixes: prefixes.into(),
        uris: uris.into(),
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

#[unsafe(no_mangle)]
pub extern "C" fn rust_namespace_context_release(context: OwnedArc<NativeNamespaceContext>) {
    drop(context);
}

static NEXT_RULE_IDENTITY: AtomicU64 = AtomicU64::new(1);

fn reserve_rule_identities(count: u64) -> u64 {
    NEXT_RULE_IDENTITY
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(count))
        .expect("rule identity overflow")
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
pub extern "C" fn rust_rule_retain(rule: RcRef<'_, NativeRule>) -> *const NativeRule {
    Rc::into_raw(rule.to_rc())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_release(rule: OwnedRc<NativeRule>) {
    drop(rule);
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
    let mut parent = rule.parent();
    while let Some(rule) = parent {
        if rule.rule_type == NativeRuleType::Function {
            return Rc::as_ptr(&rule);
        }
        parent = rule.parent();
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
    sheet_rules: Option<&NativeRuleList>,
) -> bool {
    input
        .units()
        .is_some_and(|input| rule.set_selector_text(input, sheet_rules))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_selectors(rule: &NativeRule) -> *const c_void {
    Arc::as_ptr(&rule.style_rule().selectors()).cast()
}

/// Call only for style or nested-declaration rules. The returned list must be released with
/// `rust_parsed_selector_list_destroy` on the same thread.
#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_matching_selectors(rule: &NativeRule) -> *mut c_void {
    Box::into_raw(Box::new(RustParsedSelectorList {
        selectors: rule.absolutized_selectors(),
    }))
    .cast()
}

/// Call for a scope rule or scoped import. A non-null result must be destroyed with
/// `rust_parsed_selector_list_destroy`; null means the start is omitted.
#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_scope_start_selectors(rule: &NativeRule) -> *mut c_void {
    rule.absolutized_scope_start()
        .map_or(std::ptr::null_mut(), |selectors| {
            Box::into_raw(Box::new(RustParsedSelectorList { selectors })).cast()
        })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_scope_end_selectors(rule: &NativeRule) -> *mut c_void {
    rule.absolutized_scope_end().map_or(std::ptr::null_mut(), |selectors| {
        Box::into_raw(Box::new(RustParsedSelectorList { selectors })).cast()
    })
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

fn rule_external_memory_size(rule: &NativeRule) -> usize {
    let payload_size = match &rule.payload {
        RulePayload::FontFace(_) | RulePayload::Page { .. } | RulePayload::FunctionDeclarations(_) => {
            size_of::<FfiDescriptorBlock>()
        }
        RulePayload::Keyframes(name) => {
            let mut size = size_of_val(name.borrow().units());
            let _ = rule.child_rules().unwrap().visit_rules(&mut |frame| {
                let (keys, declarations) = frame.keyframe();
                size = size
                    .saturating_add(size_of::<crate::css::keyframes::KeyframeData>())
                    .saturating_add(size_of_val(keys))
                    .saturating_add(declarations.heap_bytes());
                std::ops::ControlFlow::Continue(())
            });
            size
        }
        RulePayload::Keyframe(_) => size_of::<FfiKeyframe>(),
        RulePayload::Margin { name, .. } | RulePayload::PositionTry { name, .. } => {
            size_of::<DeclarationBlock>().saturating_add(size_of_val(name.units()))
        }
        RulePayload::CounterStyle(_) => size_of::<FfiCounterStyle>(),
        RulePayload::FontFeatureValues(_) => size_of::<FontFeatureValuesRule>(),
        RulePayload::NestedDeclarations(_) => size_of::<DeclarationBlock>(),
        _ => 0,
    };
    // Backing data is accounted for by the typed payload consumers.
    size_of::<NativeRule>().saturating_add(payload_size)
}

/// # Safety
/// The caller must not set a keyframes rule's name while it uses the returned name view.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_rule_payload(rule: &NativeRule) -> FfiRulePayload<'_> {
    let mut result = FfiRulePayload::default();
    if let Some((line, column)) = rule.source_position {
        result.has_source_position = true;
        result.start_line = line as usize;
        result.start_column = column as usize;
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
            // SAFETY: The caller reads the payload before it next sets the name.
            let name = unsafe { name.try_borrow_unguarded() }.expect("the name is not being set");
            result.name = FfiUtf16View::from_utf16(name.units());
        }
        RulePayload::Keyframe(data) => result.keyframe = &raw const **data,
        RulePayload::Margin { name, declarations } | RulePayload::PositionTry { name, declarations } => {
            result.name = FfiUtf16View::from_utf16(name.units());
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
        RulePayload::Scope(data) => result.scope = Arc::as_ptr(&data.borrow()),
    }
    result
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_set_page_selectors(rule: &NativeRule, selectors: ArcRef<'_, PageSelectorList>) {
    let RulePayload::Page { selectors: current, .. } = &rule.payload else {
        unreachable!()
    };
    *current.borrow_mut() = selectors.to_arc();
}

/// The text the list's sheet was parsed from, when the list holds a parsed sheet, which the parse
/// shares between the documents it was parsed for.
#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_source_text(list: &NativeRuleList, has_text: &mut bool) -> usize {
    let parsed = list.parsed_source.borrow();
    let Some(sheet) = parsed.as_ref().and_then(|parsed| parsed.root_sheet()) else {
        *has_text = false;
        return ak::Utf16String::from_utf16(&[]).into_raw();
    };
    *has_text = true;
    match sheet.source_text().units() {
        crate::css::css_tokenizer::TokenizerInput::Latin1(bytes) => ak::Utf16String::from_latin1(bytes).into_raw(),
        crate::css::css_tokenizer::TokenizerInput::Utf16(units) => ak::Utf16String::from_utf16(units).into_raw(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_children(rule: &NativeRule) -> *const NativeRuleList {
    rule.child_rules().map_or(std::ptr::null(), Rc::as_ptr)
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
pub extern "C" fn rust_rule_list_retain(list: RcRef<'_, NativeRuleList>) -> *const NativeRuleList {
    Rc::into_raw(list.to_rc())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_release(list: OwnedRc<NativeRuleList>) {
    drop(list);
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
    list.for_each_namespace(|namespace| unsafe {
        callback(
            context,
            FfiUtf16View::from_utf16(namespace.prefix.units()),
            FfiUtf16View::from_utf16(namespace.uri.units()),
        );
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
pub extern "C" fn rust_rule_list_insert(list: &NativeRuleList, index: usize, rule: RcRef<'_, NativeRule>) {
    list.insert(index, rule.to_rc());
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
#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_list_insert_css_rule(
    list: &NativeRuleList,
    index: usize,
    rule: RcRef<'_, NativeRule>,
    nested: bool,
) -> RuleInsertionResult {
    list.insert_css_rule(index, rule.to_rc(), nested)
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
