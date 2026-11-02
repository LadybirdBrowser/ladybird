/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{CompilationContext, NativeRuleType, NativeStyleSheet};
use crate::css::rule::read::RuleRef;
use crate::css::selector::SelectorListRef;
use crate::css::selector_operations::scope_root_selector_list;
use crate::css::selector_parser::RustParsedSelectorList;
use crate::css::style::bridge::{BoundScopeChain, declares_transitions};
use crate::css::style::engine_calls::document_host;
use crate::css::style::pipeline::rule_store::RuleKind;
use crate::css::style::rule_writes::{BoundSelectors, CompiledRule, CompiledRuleKind, RuleWrite};
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

/// What the rules a traversal reaches are directly inside, as their nested declarations see it.
#[derive(Clone, Default)]
enum Enclosing {
    #[default]
    None,
    Scope,
    /// A style rule, with its selectors.
    Style(Arc<RustParsedSelectorList>),
}

// Binding belongs to this document-thread traversal, not to the shared rule graph.
#[derive(Clone, Default)]
pub(super) struct SelectorInputs {
    enclosing: Enclosing,
    /// The scopes the rules are inside.
    scope: Arc<BoundScopeChain>,
}

impl SelectorInputs {
    /// The selectors the engine matches `rule` by, whose `&` names what the sheet's arena says.
    pub(super) fn matching_selectors(&self, rule: RuleRef<'_>) -> Option<Arc<RustParsedSelectorList>> {
        match rule.rule_type() {
            NativeRuleType::Style => rule.selectors(),
            NativeRuleType::NestedDeclarations => match &self.enclosing {
                Enclosing::Style(selectors) => Some(selectors.clone()),
                Enclosing::Scope => Some(Arc::new(RustParsedSelectorList {
                    selectors: SelectorListRef::pack(&scope_root_selector_list()),
                })),
                Enclosing::None => unreachable!("nested declarations are inside a style rule or `@scope`"),
            },
            _ => None,
        }
    }

    /// The scopes the rules are inside.
    pub(super) fn scope(&self) -> Arc<BoundScopeChain> {
        self.scope.clone()
    }

    pub(super) fn within(
        &self,
        rule: RuleRef<'_>,
        matching: Option<Arc<RustParsedSelectorList>>,
        implicit_root: u32,
    ) -> Self {
        let mut nested = self.clone();
        if rule.rule_type() == NativeRuleType::Style
            && let Some(selectors) = matching.or_else(|| self.matching_selectors(rule))
        {
            nested.enclosing = Enclosing::Style(selectors);
        }
        if let Some(scope) = rule.scope() {
            Arc::make_mut(&mut nested.scope).push(scope.start.as_deref(), scope.end.as_deref(), implicit_root);
        }
        match rule.rule_type() {
            NativeRuleType::Scope => nested.enclosing = Enclosing::Scope,
            NativeRuleType::Import => nested.enclosing = Enclosing::None,
            _ => {}
        }
        nested
    }
}

/// The rules one compilation publishes to a sheet of a document's style engine, ahead of one rule, which it writes
/// as one write as it ends. The host notes each rule it publishes as it compiles it.
pub(super) struct Publisher<'a> {
    /// The document whose engine the rules go to, or none where the process keeps them.
    host: Option<&'a DocumentHost>,
    sheet: u32,
    before: u64,
    rules: RefCell<Vec<CompiledRule>>,
}

impl<'a> Publisher<'a> {
    /// # Safety
    ///
    /// The publication's host must be a live document host, on its document's thread, which outlives the publisher.
    pub(super) unsafe fn new(publication: &NativeStylePublication) -> Self {
        Self {
            // SAFETY: Guaranteed by the caller.
            host: Some(unsafe { document_host(publication.host) }),
            sheet: publication.sheet,
            before: publication.before,
            rules: RefCell::default(),
        }
    }

    /// A publisher of a user agent sheet, whose rules the process keeps, once: no document notes them.
    pub(super) fn of_user_agent_sheet() -> Self {
        Self {
            host: None,
            sheet: 0,
            before: 0,
            rules: RefCell::default(),
        }
    }

    /// The rules the compilation published, of a publisher of a user agent sheet.
    pub(super) fn into_rules(self) -> Vec<CompiledRule> {
        debug_assert!(self.host.is_none());
        self.rules.into_inner()
    }

    pub(super) fn host(&self) -> &'a DocumentHost {
        self.host.expect("only the rules of a document's sheet are edited")
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
        }
    }

    /// Writes the rules the compilation published, as one write.
    pub(super) fn finish(self) {
        let host = self.host();
        let rules = self.rules.into_inner();
        if rules.is_empty() {
            return;
        }
        host.write_rules(RuleWrite::PublishRules {
            sheet: self.sheet,
            before: self.before,
            rules,
        });
    }

    /// Writes the selectors `selectors` for the published style rule `rule`, which keeps its place and declarations.
    pub(super) fn replace_selectors(
        &self,
        rule: RuleRef<'_>,
        context: &CompilationContext,
        selectors: Arc<RustParsedSelectorList>,
    ) {
        self.host().write_rules(RuleWrite::ReplaceSelectors {
            identity: rule.identity(),
            selectors: BoundSelectors {
                selectors,
                scope: context.selectors.scope(),
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
                        scope: context.selectors.scope(),
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
                .is_some_and(|declarations| declares_transitions(declarations.properties()));
        let in_a_layer = context.in_a_layer
            && matches!(
                rule.rule_type(),
                NativeRuleType::Style | NativeRuleType::NestedDeclarations | NativeRuleType::CounterStyle
            );
        if let Some(host) = self.host {
            host.published_rules().insert(self.sheet, rule.identity());
            // NB: These rules hand over declarations the rule write does not hold.
            match rule.rule_type() {
                NativeRuleType::Function => super::super::function::note_attr_names_read_by_function(host, rule),
                NativeRuleType::Keyframes => {
                    let _ = rule.visit_children(&mut |keyframe| {
                        let (_, declarations) = keyframe.keyframe();
                        crate::css::style::host_atoms::note_attr_names_read_by_block(host, &declarations);
                        std::ops::ControlFlow::Continue(())
                    });
                }
                _ => {}
            }
        }
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
