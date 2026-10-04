/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{CompilationContext, NativeRuleType, NativeStyleSheet};
use crate::css::rule::read::RuleRef;
use crate::css::selector_operations::{
    absolutize_selector_list, adapt_scope_end_selector_list, scope_root_selector_list,
};
use crate::css::selector_parser::{RustParsedSelectorList, StyleNestingParent};
use crate::css::style::bridge::{BoundScopeChain, declares_transitions};
use crate::css::style::engine_calls::document_host;
use crate::css::style::program::RuleKind;
use crate::css::style::rule_writes::{BoundSelectors, CompiledRule, CompiledRuleKind, NamespaceTexts, RuleWrite};
use crate::render_state::DocumentHost;
use std::cell::RefCell;
use std::sync::Arc;

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct NativeCompilationResult {
    pub published: bool,
    pub declares_transitions: bool,
}

// All pointers are borrowed for main-thread compilation. Neither the parser nor the native
// stylesheet graph retains document hosts, interners, or host callbacks.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NativeStylePublication {
    /// The host of the document whose style engine the rules are published to.
    pub host: *const DocumentHost,
    pub sheet: u32,
    /// The native identity of the published rule the rules go before, or 0 for the end of the sheet.
    pub before: u64,
}

// Binding belongs to this document-thread traversal, not to the shared rule graph.
#[derive(Clone)]
pub(super) struct SelectorInputs {
    parent_kind: StyleNestingParent,
    immediate_parent_kind: StyleNestingParent,
    parents: Option<Arc<RustParsedSelectorList>>,
    scope: Arc<BoundScopeChain>,
}

impl Default for SelectorInputs {
    fn default() -> Self {
        Self {
            parent_kind: StyleNestingParent::None,
            immediate_parent_kind: StyleNestingParent::None,
            parents: None,
            scope: Arc::default(),
        }
    }
}

impl SelectorInputs {
    unsafe fn bind(
        &self,
        source: &RustParsedSelectorList,
        parent_kind: StyleNestingParent,
    ) -> Arc<RustParsedSelectorList> {
        let empty: crate::css::selector::SelectorList = Box::new([]);
        let parents = if parent_kind == StyleNestingParent::Style {
            self.parents.as_ref().map_or(&empty, |parents| &parents.selectors)
        } else {
            &empty
        };
        Arc::new(RustParsedSelectorList {
            selectors: absolutize_selector_list(&source.selectors, parent_kind, parents)
                .unwrap_or_else(|| source.selectors.clone()),
        })
    }

    pub(super) unsafe fn matching_selectors(&self, rule: RuleRef<'_>) -> Option<Arc<RustParsedSelectorList>> {
        match rule.rule_type() {
            NativeRuleType::Style => Some(unsafe { self.bind(&rule.selectors().unwrap(), self.parent_kind) }),
            NativeRuleType::NestedDeclarations => {
                if let Some(parents) = &self.parents {
                    return Some(parents.clone());
                }
                assert_eq!(self.parent_kind, StyleNestingParent::Scope);
                Some(Arc::new(RustParsedSelectorList {
                    selectors: scope_root_selector_list(),
                }))
            }
            _ => None,
        }
    }

    pub(super) unsafe fn within(
        &self,
        rule: RuleRef<'_>,
        matching: Option<Arc<RustParsedSelectorList>>,
        implicit_root: u32,
    ) -> Self {
        let mut nested = self.clone();
        nested.immediate_parent_kind = StyleNestingParent::None;
        if rule.rule_type() == NativeRuleType::Style {
            nested.parents = matching.or_else(|| unsafe { self.matching_selectors(rule) });
            nested.parent_kind = StyleNestingParent::Style;
            nested.immediate_parent_kind = StyleNestingParent::Style;
        }
        if let Some(scope) = rule.scope() {
            let parent_kind = if rule.rule_type() == NativeRuleType::Import {
                StyleNestingParent::None
            } else {
                self.immediate_parent_kind
            };
            let start = scope
                .start
                .as_ref()
                .map(|start| unsafe { self.bind(start, parent_kind) });
            let end = scope.end.as_ref().map(|end| RustParsedSelectorList {
                selectors: adapt_scope_end_selector_list(&end.selectors),
            });
            Arc::make_mut(&mut nested.scope).push(start.as_deref(), end.as_ref(), implicit_root);
        }
        match rule.rule_type() {
            NativeRuleType::Scope => {
                nested.parent_kind = StyleNestingParent::Scope;
                nested.immediate_parent_kind = StyleNestingParent::Scope;
                nested.parents = None;
            }
            NativeRuleType::Import => {
                nested.parent_kind = StyleNestingParent::None;
                nested.parents = None;
            }
            _ => {}
        }
        nested
    }
}

/// The rules one compilation publishes to a sheet of a document's style engine, ahead of one rule, which it writes
/// as one write as it ends. The host notes each rule it publishes as it compiles it.
pub(super) struct Publisher<'a> {
    host: &'a DocumentHost,
    sheet: u32,
    before: u64,
    rules: RefCell<Vec<CompiledRule>>,
    /// The native sheet the last rule came from, by identity, and the namespaces it declares.
    namespaces: RefCell<Option<(u64, Option<Arc<NamespaceTexts>>)>>,
}

impl<'a> Publisher<'a> {
    /// # Safety
    ///
    /// The publication's host must be a live document host, on its document's thread, which outlives the publisher.
    pub(super) unsafe fn new(publication: &NativeStylePublication) -> Self {
        Self {
            // SAFETY: Guaranteed by the caller.
            host: unsafe { document_host(publication.host) },
            sheet: publication.sheet,
            before: publication.before,
            rules: RefCell::default(),
            namespaces: RefCell::default(),
        }
    }

    pub(super) fn host(&self) -> &'a DocumentHost {
        self.host
    }

    pub(super) fn sheet(&self) -> u32 {
        self.sheet
    }

    /// A publisher of the same sheet, whose rules go before the rule `before` names.
    pub(super) fn before(&self, before: u64) -> Self {
        Self {
            host: self.host,
            sheet: self.sheet,
            before,
            rules: RefCell::default(),
            namespaces: RefCell::default(),
        }
    }

    /// Writes the rules the compilation published, as one write.
    pub(super) fn finish(self) {
        let rules = self.rules.into_inner();
        if rules.is_empty() {
            return;
        }
        self.host.write_rules(RuleWrite::PublishRules {
            sheet: self.sheet,
            before: self.before,
            rules,
        });
    }

    /// The namespaces `source` declares, which the rules that come from it share.
    fn namespaces_of(&self, source: &NativeStyleSheet) -> Option<Arc<NamespaceTexts>> {
        let mut cached = self.namespaces.borrow_mut();
        if let Some((identity, texts)) = &*cached
            && *identity == source.identity()
        {
            return texts.clone();
        }
        let texts = NamespaceTexts::of(source);
        *cached = Some((source.identity(), texts.clone()));
        texts
    }

    /// Writes the selectors `selectors` for the published style rule `rule`, which keeps its place and declarations.
    pub(super) fn replace_selectors(
        &self,
        rule: RuleRef<'_>,
        source: &NativeStyleSheet,
        context: &CompilationContext,
        selectors: Arc<RustParsedSelectorList>,
    ) {
        self.host.write_rules(RuleWrite::ReplaceSelectors {
            identity: rule.identity(),
            selectors: BoundSelectors {
                selectors,
                scope: context.selectors.scope.clone(),
                namespaces: self.namespaces_of(source),
            },
        });
    }

    /// Compiles `rule`, of `source`, into the rules the compilation publishes, where it is a rule the engine has, and
    /// answers whether it is one and whether it declares transitions.
    pub(super) fn compile(
        &self,
        rule: RuleRef<'_>,
        source: &NativeStyleSheet,
        context: &CompilationContext,
        selectors: Option<&Arc<RustParsedSelectorList>>,
    ) -> NativeCompilationResult {
        let kind = match rule.rule_type() {
            NativeRuleType::Style | NativeRuleType::NestedDeclarations => {
                let selectors = selectors.unwrap();
                if selectors.selectors.is_empty() {
                    return NativeCompilationResult::default();
                }
                CompiledRuleKind::Style {
                    selectors: BoundSelectors {
                        selectors: selectors.clone(),
                        scope: context.selectors.scope.clone(),
                        namespaces: self.namespaces_of(source),
                    },
                    gated_by_container_query: context.gated_by_container_query,
                }
            }
            NativeRuleType::FontFeatureValues => CompiledRuleKind::NonMatching(RuleKind::FontFeatureValues),
            NativeRuleType::CounterStyle => CompiledRuleKind::NonMatching(RuleKind::CounterStyle),
            NativeRuleType::Function => CompiledRuleKind::NonMatching(RuleKind::Function),
            NativeRuleType::Property => CompiledRuleKind::Named {
                kind: RuleKind::Property,
                name: rule.definition_name().unwrap().units().into(),
            },
            NativeRuleType::Keyframes => CompiledRuleKind::Named {
                kind: RuleKind::Keyframes,
                name: rule.definition_name().unwrap().units().into(),
            },
            _ => return NativeCompilationResult::default(),
        };
        let declarations = rule.cascade_declarations();
        let declares_transitions = matches!(kind, CompiledRuleKind::Style { .. })
            && declarations
                .as_ref()
                .is_some_and(|declarations| declares_transitions(&declarations.properties));
        let in_a_layer = context.in_a_layer
            && matches!(
                rule.rule_type(),
                NativeRuleType::Style | NativeRuleType::NestedDeclarations | NativeRuleType::CounterStyle
            );
        self.host.published_rules().insert(self.sheet, rule.identity());
        self.rules.borrow_mut().push(CompiledRule {
            identity: rule.identity(),
            kind,
            declarations,
            source_identity: source.identity(),
            conditions_hold: context.conditions_hold,
            in_a_layer,
            layer_name: context.layer_name.clone(),
            containers: context
                .containers
                .iter()
                .rev()
                .map(|&container| {
                    // SAFETY: The rule the traversal borrows holds the container conditions, as an Arc.
                    unsafe {
                        Arc::increment_strong_count(container);
                        Arc::from_raw(container)
                    }
                })
                .collect(),
        });
        NativeCompilationResult {
            published: true,
            declares_transitions,
        }
    }
}
