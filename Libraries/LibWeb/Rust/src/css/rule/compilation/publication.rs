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
use crate::css::style::bridge::{
    BoundScopeChain, operations, publish_rule_declarations, publish_style_rule, publish_style_rule_selectors,
    publish_user_agent_style_rule,
};
use crate::css::style::compiler::NamespaceScope;
use crate::css::style::engine_calls::{sheet_writing_host, with_engine};
use crate::render_state::{BegunRead, DocumentHost};
use std::rc::Rc;

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct NativeCompilationResult {
    pub rule_id: u32,
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
    pub before_rule: u32,
}

/// A publication the host makes in a read it began, which reaches the style engine in place.
#[derive(Clone, Copy)]
pub(super) struct Publication<'a> {
    pub(super) to: NativeStylePublication,
    pub(super) read: &'a BegunRead,
}

// Binding belongs to this document-thread traversal, not to the shared rule graph.
#[derive(Clone)]
pub(super) struct SelectorInputs {
    parent_kind: StyleNestingParent,
    immediate_parent_kind: StyleNestingParent,
    parents: Option<Rc<RustParsedSelectorList>>,
    scope: BoundScopeChain,
}

impl Default for SelectorInputs {
    fn default() -> Self {
        Self {
            parent_kind: StyleNestingParent::None,
            immediate_parent_kind: StyleNestingParent::None,
            parents: None,
            scope: BoundScopeChain::default(),
        }
    }
}

impl SelectorInputs {
    unsafe fn bind(
        &self,
        source: &RustParsedSelectorList,
        parent_kind: StyleNestingParent,
    ) -> Rc<RustParsedSelectorList> {
        let empty: crate::css::selector::SelectorList = Box::new([]);
        let parents = if parent_kind == StyleNestingParent::Style {
            self.parents.as_ref().map_or(&empty, |parents| &parents.selectors)
        } else {
            &empty
        };
        Rc::new(RustParsedSelectorList {
            selectors: absolutize_selector_list(&source.selectors, parent_kind, parents)
                .unwrap_or_else(|| source.selectors.clone()),
        })
    }

    pub(super) unsafe fn matching_selectors(&self, rule: RuleRef<'_>) -> Option<Rc<RustParsedSelectorList>> {
        match rule.rule_type() {
            NativeRuleType::Style => Some(unsafe { self.bind(&rule.selectors().unwrap(), self.parent_kind) }),
            NativeRuleType::NestedDeclarations => {
                if let Some(parents) = &self.parents {
                    return Some(parents.clone());
                }
                assert_eq!(self.parent_kind, StyleNestingParent::Scope);
                Some(Rc::new(RustParsedSelectorList {
                    selectors: scope_root_selector_list(),
                }))
            }
            _ => None,
        }
    }

    pub(super) unsafe fn within(
        &self,
        rule: RuleRef<'_>,
        matching: Option<Rc<RustParsedSelectorList>>,
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
            nested.scope.push(start.as_deref(), end.as_ref(), implicit_root);
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

impl Publication<'_> {
    /// The host of the document whose style engine the rules are published to, behind the drain of its style
    /// transaction that flew.
    ///
    /// # Safety
    ///
    /// The publication's host must be a live document host, on its document's thread.
    pub(super) unsafe fn host(&self) -> &DocumentHost {
        // SAFETY: Guaranteed by the caller.
        unsafe { sheet_writing_host(self.to.host, self.read) }
    }

    pub(super) unsafe fn replace_selectors(
        &self,
        rule: RuleRef<'_>,
        source: &NativeStyleSheet,
        context: &CompilationContext,
        selectors: &RustParsedSelectorList,
    ) {
        // SAFETY: Guaranteed by the caller.
        with_engine(self.read, unsafe { self.host() }, |engine| {
            let id = engine.native_rule_id(rule.identity()).map_or(0, |id| id.0 + 1);
            let namespaces = NamespaceScope::from_rule_list(source.rules(), |text| {
                crate::css::style::bridge::intern_native_text(engine, text)
            });
            let compiled: Vec<_> = selectors.selectors.iter().map(|selector| selector.as_ref()).collect();
            publish_style_rule_selectors(engine, id, &compiled, namespaces, &context.selectors.scope);
        });
    }

    pub(super) unsafe fn compile(
        &self,
        rule: RuleRef<'_>,
        source: &NativeStyleSheet,
        context: &CompilationContext,
        selectors: Option<&RustParsedSelectorList>,
    ) -> NativeCompilationResult {
        // SAFETY: Guaranteed by the caller.
        with_engine(self.read, unsafe { self.host() }, |engine| {
            let mut result = NativeCompilationResult::default();
            let sheet = self.to.sheet;
            let before = self.to.before_rule;
            // Reuse the engine's recorded publication operations so recording and replay see the
            // same semantic inputs as incremental CSSOM edits.
            result.rule_id = match rule.rule_type() {
                NativeRuleType::Style | NativeRuleType::NestedDeclarations => {
                    let selectors = selectors.unwrap();
                    if selectors.selectors.is_empty() {
                        return result;
                    }
                    let compiled: Vec<_> = selectors.selectors.iter().map(|selector| selector.as_ref()).collect();
                    let scope = &context.selectors.scope;
                    let id = publish_user_agent_style_rule(
                        engine,
                        sheet,
                        before,
                        rule.identity(),
                        &compiled,
                        source.rules(),
                        scope,
                    )
                    .unwrap_or_else(|| {
                        let namespaces = NamespaceScope::from_rule_list(source.rules(), |text| {
                            crate::css::style::bridge::intern_native_text(engine, text)
                        });
                        publish_style_rule(engine, sheet, before, &compiled, namespaces, scope)
                    });
                    let declarations = rule.cascade_declarations().unwrap();
                    result.declares_transitions = publish_rule_declarations(engine, id, &declarations);
                    if context.gated_by_container_query {
                        operations::set_rule_gated_by_container_query(engine, id);
                    }
                    id
                }
                NativeRuleType::FontFeatureValues => operations::add_font_feature_values_rule(engine, sheet, before),
                NativeRuleType::CounterStyle => operations::add_counter_style_rule(engine, sheet, before),
                NativeRuleType::Function => operations::add_function_rule(engine, sheet, before),
                NativeRuleType::Property => {
                    let name =
                        crate::css::style::bridge::intern_native_text(engine, rule.definition_name().unwrap().units())
                            .0;
                    operations::add_property_rule(engine, sheet, before, name)
                }
                NativeRuleType::Keyframes => {
                    let name =
                        crate::css::style::bridge::intern_native_text(engine, rule.definition_name().unwrap().units())
                            .0;
                    operations::add_keyframes_rule(engine, sheet, before, name)
                }
                _ => 0,
            };
            if result.rule_id != 0 {
                if !context.conditions_hold {
                    operations::set_rule_conditions_hold(engine, result.rule_id, false);
                }
                if context.in_a_layer
                    && matches!(
                        rule.rule_type(),
                        NativeRuleType::Style | NativeRuleType::NestedDeclarations | NativeRuleType::CounterStyle
                    )
                {
                    let layer = crate::css::style::bridge::intern_native_text(engine, &context.layer_name).0;
                    operations::set_rule_in_a_layer(engine, result.rule_id);
                    operations::set_rule_layer(engine, result.rule_id, layer);
                }
            }
            if result.rule_id != 0 {
                unsafe {
                    engine.register_native_rule(
                        crate::css::style::program::RuleID(result.rule_id - 1),
                        rule.identity(),
                        rule.cascade_declarations(),
                        source.identity(),
                        &context.layer_name,
                        &context.containers,
                    );
                }
            }
            result
        })
    }
}
