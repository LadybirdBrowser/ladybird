/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::read::RuleRef;
use super::{NativeRule, NativeRuleList, NativeRuleType};
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::query_parser::{FfiMediaEnvironment, MediaEnvironment};
use crate::css::style_sheet::NativeStyleSheet;
use std::ffi::c_void;
use std::rc::Rc;

mod publication;
use publication::{NativeCompilationResult, NativeStylePublication, SelectorInputs};

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum NativeCompilationPurpose {
    Rules,
    Selectors,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct NativeCompilationScope {
    pub identity: u64,
    pub implicit_root: u32,
}

#[repr(C)]
pub struct NativeCompilationContext {
    pub layer_name: FfiUtf16View,
    pub scopes: *const NativeCompilationScope,
    pub scope_count: usize,
    pub containers: *const *const ContainerConditionsData,
    pub container_count: usize,
    pub conditions_hold: bool,
    pub in_a_layer: bool,
    pub gated_by_container_query: bool,
}

#[repr(C)]
pub struct NativeCompilationCallbacks {
    pub context: *const c_void,
    pub import_source: unsafe extern "C" fn(*const c_void, u64, &NativeStyleSheet) -> *const c_void,
    pub implicit_scope_root: unsafe extern "C" fn(*const c_void) -> u32,
    pub visit_rule: unsafe extern "C" fn(
        *const c_void,
        *const c_void,
        u64,
        NativeRuleType,
        &NativeCompilationContext,
        NativeCompilationResult,
    ) -> bool,
}

// These are borrowed traversal inputs, never fields of the parsed graph. Only scope roots and
// the identity of each source's host-side loading state come from the document.
#[derive(Clone)]
struct CompilationContext {
    selectors: SelectorInputs,
    purpose: NativeCompilationPurpose,
    layer_name: Vec<u16>,
    scopes: Vec<NativeCompilationScope>,
    containers: Vec<*const ContainerConditionsData>,
    conditions_hold: bool,
    in_a_layer: bool,
    gated_by_container_query: bool,
}

impl Default for CompilationContext {
    fn default() -> Self {
        Self {
            selectors: SelectorInputs::default(),
            purpose: NativeCompilationPurpose::Rules,
            layer_name: Vec::new(),
            scopes: Vec::new(),
            containers: Vec::new(),
            conditions_hold: true,
            in_a_layer: false,
            gated_by_container_query: false,
        }
    }
}

impl CompilationContext {
    fn view(&self) -> NativeCompilationContext {
        NativeCompilationContext {
            layer_name: FfiUtf16View {
                ascii: std::ptr::null(),
                utf16: self.layer_name.as_ptr(),
                length: self.layer_name.len(),
            },
            scopes: self.scopes.as_ptr(),
            scope_count: self.scopes.len(),
            containers: self.containers.as_ptr(),
            container_count: self.containers.len(),
            conditions_hold: self.conditions_hold,
            in_a_layer: self.in_a_layer,
            gated_by_container_query: self.gated_by_container_query,
        }
    }

    unsafe fn within(
        &self,
        rule: RuleRef<'_>,
        source: *const c_void,
        environment: MediaEnvironment<'_>,
        callbacks: &NativeCompilationCallbacks,
        publication: Option<&NativeStylePublication>,
        matching: Option<Rc<crate::css::selector_parser::RustBoundSelectorList>>,
    ) -> Self {
        let mut context = self.clone();
        if self.purpose == NativeCompilationPurpose::Rules {
            context.conditions_hold = self.conditions_hold && rule.condition_holds(environment);
        }
        if let Some(name) = rule.internal_layer_name() {
            if !context.layer_name.is_empty() {
                context.layer_name.push(u16::from(b'.'));
            }
            context.layer_name.extend_from_slice(&name);
            context.in_a_layer = true;
        }
        let mut implicit_root = 0;
        if rule.scope().is_some() {
            implicit_root = unsafe { (callbacks.implicit_scope_root)(source) };
            context.scopes.push(NativeCompilationScope {
                identity: rule.identity(),
                implicit_root,
            });
        }
        if publication.is_some() {
            context.selectors = unsafe { self.selectors.within(rule, matching, implicit_root) };
        }
        if let Some(container) = rule.container() {
            context.containers.push(std::sync::Arc::as_ptr(container));
            context.gated_by_container_query = true;
        } else if rule.rule_type() == NativeRuleType::Import {
            context.containers.clear();
        }
        context
    }
}

fn has_compiled_children(rule: RuleRef<'_>) -> bool {
    !matches!(
        rule.rule_type(),
        NativeRuleType::FontFeatureValues
            | NativeRuleType::CounterStyle
            | NativeRuleType::Function
            | NativeRuleType::LayerStatement
            | NativeRuleType::Property
            | NativeRuleType::Keyframes
    )
}

unsafe fn visit_rule(
    rule: RuleRef<'_>,
    sheet: &NativeStyleSheet,
    source: *const c_void,
    context: &CompilationContext,
    environment: MediaEnvironment<'_>,
    callbacks: &NativeCompilationCallbacks,
    publication: Option<&NativeStylePublication>,
) {
    let selectors = publication.and_then(|_| unsafe { context.selectors.matching_selectors(rule) });
    if context.purpose == NativeCompilationPurpose::Selectors
        && let Some(publication) = publication
    {
        unsafe { publication.replace_selectors(rule, sheet, context, selectors.as_deref().unwrap()) };
        return;
    }
    let result = publication.map_or_else(NativeCompilationResult::default, |publication| unsafe {
        publication.compile(rule, sheet, context, selectors.as_deref())
    });
    if !unsafe {
        (callbacks.visit_rule)(
            callbacks.context,
            source,
            rule.identity(),
            rule.rule_type(),
            &context.view(),
            result,
        )
    } || !has_compiled_children(rule)
    {
        return;
    }
    if rule.rule_type() == NativeRuleType::Import {
        if let Some(imported) = sheet.imported_sheet(rule.identity()) {
            let nested = unsafe { context.within(rule, source, environment, callbacks, publication, selectors) };
            let imported_source = unsafe { (callbacks.import_source)(source, rule.identity(), &imported) };
            unsafe {
                visit_list(
                    imported.rules(),
                    &imported,
                    imported_source,
                    &nested,
                    environment,
                    callbacks,
                    publication,
                );
            };
        }
    } else {
        let nested = unsafe { context.within(rule, source, environment, callbacks, publication, selectors) };
        let _ = rule.visit_children(&mut |child| {
            unsafe {
                visit_rule(child, sheet, source, &nested, environment, callbacks, publication);
            }
            std::ops::ControlFlow::Continue(())
        });
    }
}

unsafe fn visit_list(
    list: &NativeRuleList,
    sheet: &NativeStyleSheet,
    source: *const c_void,
    context: &CompilationContext,
    environment: MediaEnvironment<'_>,
    callbacks: &NativeCompilationCallbacks,
    publication: Option<&NativeStylePublication>,
) {
    let _ = list.visit_rules(&mut |rule| {
        unsafe {
            visit_rule(rule, sheet, source, context, environment, callbacks, publication);
        };
        std::ops::ControlFlow::Continue(())
    });
}

struct RulePathEntry {
    rule: Rc<NativeRule>,
    imported: Option<Rc<NativeStyleSheet>>,
}

// Locate a CSSOM mutation without evaluating unrelated conditional groups. The path also keeps
// imported sources alive while deriving the same context as whole-sheet compilation.
fn find_rule_path(
    list: &NativeRuleList,
    sheet: &NativeStyleSheet,
    identity: u64,
    path: &mut Vec<RulePathEntry>,
) -> bool {
    for rule in list.materialized_rules().iter() {
        let imported = sheet.imported_sheet(rule.identity);
        path.push(RulePathEntry {
            rule: rule.clone(),
            imported: imported.clone(),
        });
        if rule.identity == identity {
            return true;
        }
        let found = if let Some(imported) = imported {
            find_rule_path(imported.rules(), &imported, identity, path)
        } else {
            rule.children
                .as_ref()
                .is_some_and(|children| find_rule_path(children, sheet, identity, path))
        };
        if found {
            return true;
        }
        path.pop();
    }
    false
}

/// Derive compilation inputs directly from the native sheet graph, including imported sheets.
/// A zero rule identity visits the whole sheet; otherwise only that rule and its subtree are
/// visited, with the same inherited context. Returning false from visit_rule skips its children.
/// Selector replacement derives ancestor scopes without reevaluating media conditions.
///
/// # Safety
/// The environment, source, and callbacks must remain valid for this call. Callbacks must not
/// mutate the graph and must retain any native data they keep beyond a callback. import_source
/// must return the host loading state corresponding to the supplied native imported sheet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_visit_compilation(
    sheet: &NativeStyleSheet,
    rule_identity: u64,
    purpose: NativeCompilationPurpose,
    source: *const c_void,
    environment: FfiMediaEnvironment,
    callbacks: &NativeCompilationCallbacks,
) {
    unsafe {
        visit_compilation(
            sheet,
            rule_identity,
            purpose,
            source,
            environment.borrow(),
            callbacks,
            None,
        );
    };
}

/// Compile a native stylesheet or inserted subtree into its document's style program.
/// Host callbacks receive completed rule identities, not instructions to compile individual rules.
///
/// # Safety
/// The graph and callback requirements of rust_style_sheet_visit_compilation apply. Publication
/// must name a live main-thread engine and sheet. No engine borrow may span a host callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_compile(
    sheet: &NativeStyleSheet,
    rule_identity: u64,
    source: *const c_void,
    environment: FfiMediaEnvironment,
    callbacks: &NativeCompilationCallbacks,
    publication: &NativeStylePublication,
) {
    unsafe {
        visit_compilation(
            sheet,
            rule_identity,
            NativeCompilationPurpose::Rules,
            source,
            environment.borrow(),
            callbacks,
            Some(publication),
        );
    };
}

/// Update the selectors of a style rule and its native descendants without materializing CSSOM.
///
/// # Safety
/// The graph, callback, and publication requirements of rust_style_sheet_compile apply.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_replace_selectors(
    sheet: &NativeStyleSheet,
    rule_identity: u64,
    source: *const c_void,
    environment: FfiMediaEnvironment,
    callbacks: &NativeCompilationCallbacks,
    publication: &NativeStylePublication,
) {
    use crate::css::style::StyleEngine;
    let environment = unsafe { environment.borrow() };
    let mut path = Vec::new();
    if !find_rule_path(sheet.rules(), sheet, rule_identity, &mut path) {
        return;
    }
    let target = path.pop().unwrap().rule;
    let mut context = CompilationContext {
        purpose: NativeCompilationPurpose::Selectors,
        ..Default::default()
    };
    let mut target_sheet = sheet;
    let mut target_source = source;
    for entry in &path {
        context = unsafe {
            context.within(
                RuleRef::Materialized(&entry.rule),
                target_source,
                environment,
                callbacks,
                Some(publication),
                None,
            )
        };
        if let Some(imported) = &entry.imported {
            target_source = unsafe { (callbacks.import_source)(target_source, entry.rule.identity, imported) };
            target_sheet = imported;
        }
    }
    // Bind each ancestor and descendant once. Traversal inputs keep the new parent binding
    // alive for its children without storing document-bound selectors on native rule owners.
    let mut pending = vec![(target, context)];
    let mut affected = Vec::new();
    while let Some((rule, context)) = pending.pop() {
        let selectors = unsafe { context.selectors.matching_selectors(RuleRef::Materialized(&rule)) };
        if has_compiled_children(RuleRef::Materialized(&rule))
            && let Some(children) = &rule.children
        {
            let nested = unsafe {
                context.within(
                    RuleRef::Materialized(&rule),
                    target_source,
                    environment,
                    callbacks,
                    Some(publication),
                    selectors.clone(),
                )
            };
            pending.extend(
                children
                    .materialized_rules()
                    .iter()
                    .rev()
                    .map(|rule| (rule.clone(), nested.clone())),
            );
        }
        if matches!(
            rule.rule_type,
            NativeRuleType::Style | NativeRuleType::NestedDeclarations
        ) {
            affected.push((rule, context, selectors.unwrap()));
        }
    }
    for (rule, context, selectors) in affected {
        let id = unsafe { &*publication.engine.cast::<StyleEngine>() }.native_rule_id(rule.identity);
        if id.is_some() {
            unsafe { publication.replace_selectors(RuleRef::Materialized(&rule), target_sheet, &context, &selectors) };
            continue;
        }
        // A previously empty selector list may become matchable. Publish that newly active
        // subtree with its current conditions and source-order position.
        let mut publication = *publication;
        publication.before_rule = crate::css::rule::mutation::successor(sheet, rule.identity, |identity| {
            unsafe { &*publication.engine.cast::<StyleEngine>() }
                .native_rule_id(identity)
                .map_or(0, |id| id.0 + 1)
        });
        unsafe {
            visit_compilation(
                sheet,
                rule.identity,
                NativeCompilationPurpose::Rules,
                source,
                environment,
                callbacks,
                Some(&publication),
            );
        }
    }
}

unsafe fn visit_compilation(
    sheet: &NativeStyleSheet,
    rule_identity: u64,
    purpose: NativeCompilationPurpose,
    source: *const c_void,
    environment: MediaEnvironment<'_>,
    callbacks: &NativeCompilationCallbacks,
    publication: Option<&NativeStylePublication>,
) {
    let context = CompilationContext {
        purpose,
        ..Default::default()
    };
    if rule_identity == 0 {
        unsafe {
            visit_list(
                sheet.rules(),
                sheet,
                source,
                &context,
                environment,
                callbacks,
                publication,
            );
        };
        return;
    }
    #[expect(
        clippy::too_many_arguments,
        reason = "Targeted traversal carries a rule identity in addition to the compilation inputs"
    )]
    unsafe fn find(
        rule: RuleRef<'_>,
        identity: u64,
        sheet: &NativeStyleSheet,
        source: *const c_void,
        context: &CompilationContext,
        environment: MediaEnvironment<'_>,
        callbacks: &NativeCompilationCallbacks,
        publication: Option<&NativeStylePublication>,
    ) -> std::ops::ControlFlow<()> {
        if rule.identity() == identity {
            unsafe {
                visit_rule(rule, sheet, source, context, environment, callbacks, publication);
            }
            return std::ops::ControlFlow::Break(());
        }
        if !has_compiled_children(rule) {
            return std::ops::ControlFlow::Continue(());
        }
        let nested = unsafe { context.within(rule, source, environment, callbacks, publication, None) };
        if rule.rule_type() == NativeRuleType::Import {
            if let Some(imported) = sheet.imported_sheet(rule.identity()) {
                let imported_source = unsafe { (callbacks.import_source)(source, rule.identity(), &imported) };
                return imported.rules().visit_rules(&mut |child| unsafe {
                    find(
                        child,
                        identity,
                        &imported,
                        imported_source,
                        &nested,
                        environment,
                        callbacks,
                        publication,
                    )
                });
            }
            std::ops::ControlFlow::Continue(())
        } else {
            rule.visit_children(&mut |child| unsafe {
                find(
                    child,
                    identity,
                    sheet,
                    source,
                    &nested,
                    environment,
                    callbacks,
                    publication,
                )
            })
        }
    }
    let _ = sheet.rules().visit_rules(&mut |rule| unsafe {
        find(
            rule,
            rule_identity,
            sheet,
            source,
            &context,
            environment,
            callbacks,
            publication,
        )
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::media_list::MediaList;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::RulePayload;
    use crate::css::style_sheet::{rust_style_sheet_create, rust_style_sheet_set_import};
    use std::cell::RefCell;

    fn sheet(source: &str) -> Rc<NativeStyleSheet> {
        let units: Vec<_> = source.encode_utf16().collect();
        let view = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parsed = unsafe { parse_shared_stylesheet(view, &context) };
        let rules = crate::css::rule::NativeRuleList::from_parsed(parsed);
        let media = MediaList::new(Default::default());
        NativeStyleSheet::new(rules, media)
    }

    #[test]
    fn cssom_materialization_preserves_shared_rule_classification_and_children() {
        fn snapshot(rules: &NativeRuleList) -> Vec<(u64, NativeRuleType, usize)> {
            fn visit(rule: RuleRef<'_>, depth: usize, result: &mut Vec<(u64, NativeRuleType, usize)>) {
                result.push((rule.identity(), rule.rule_type(), depth));
                let _ = rule.visit_children(&mut |child| {
                    visit(child, depth + 1, result);
                    std::ops::ControlFlow::Continue(())
                });
            }
            let mut result = Vec::new();
            let _ = rules.visit_rules(&mut |rule| {
                visit(rule, 0, &mut result);
                std::ops::ControlFlow::Continue(())
            });
            result
        }
        fn materialize(rules: &NativeRuleList) {
            for rule in rules.materialized_rules().iter() {
                if let Some(children) = &rule.children {
                    materialize(children);
                }
            }
        }
        let source = sheet(
            "@layer first;
             @import 'data:text/css,';
             @namespace svg 'http://www.w3.org/2000/svg';
             @media all { .親 { & {} color: red; @supports (display: grid) { width: 1px; } } }
             @container (width > 1px) { @scope (.根) { @layer inner { .子 {} } } }
             @page :left { margin: 1px; @top-left { content: 'title'; } }
             @keyframes 動き { from { opacity: 0; } to { opacity: 1; } }
             @font-face { font-family: Test; src: local(Test); }
             @font-feature-values Test { @styleset { alternate: 1; } }
             @counter-style numbers { system: numeric; symbols: '0' '1'; }
             @property --色 { syntax: '<color>'; inherits: false; initial-value: red; }
             @function --identity(--x) { @media all { result: var(--x); } }
             @unknown ignored {}",
        );
        let before = snapshot(source.rules());
        assert!(
            before
                .iter()
                .any(|(_, kind, _)| *kind == NativeRuleType::NestedDeclarations)
        );
        assert!(
            before
                .iter()
                .any(|(_, kind, _)| *kind == NativeRuleType::FunctionDeclarations)
        );
        assert!(before.iter().any(|(_, kind, _)| *kind == NativeRuleType::Margin));
        assert!(before.iter().any(|(_, kind, _)| *kind == NativeRuleType::Keyframe));
        materialize(source.rules());
        assert_eq!(snapshot(source.rules()), before);
    }

    fn with_media_width<T>(width: f64, visit: impl FnOnce(MediaEnvironment<'_>) -> T) -> T {
        use crate::css::parser::query_parser::{
            FfiMediaFeatureValue, FfiMediaFeatureValueKind, QueryKind, resolve_query_feature,
        };
        let name: Vec<_> = "width".encode_utf16().collect();
        let id = usize::from(resolve_query_feature(QueryKind::Media, &name).unwrap().0);
        let mut values = vec![
            FfiMediaFeatureValue {
                kind: FfiMediaFeatureValueKind::Absent,
                keyword: 0,
                value: 0.0,
                second_value: 0.0
            };
            id + 1
        ];
        values[id].kind = FfiMediaFeatureValueKind::Length;
        values[id].value = width;
        let environment = FfiMediaEnvironment {
            values: values.as_ptr(),
            value_count: values.len(),
            length_resolution_context: std::ptr::null(),
        };
        visit(unsafe { environment.borrow() })
    }

    fn media_at(sheet: &NativeStyleSheet, path: &[usize]) -> MediaList {
        let mut rule = sheet.rules().rule_at(path[0]);
        for &index in &path[1..] {
            rule = rule.children.as_ref().unwrap().rule_at(index);
        }
        match &rule.payload {
            RulePayload::Media { list, .. } | RulePayload::Import { media: list, .. } => list.clone(),
            _ => panic!("expected media"),
        }
    }

    #[test]
    fn media_query_traversal_preserves_conditional_baselines() {
        let text = "@media (min-width: 100px) { @media (min-width: 200px) {} } @supports (unknown: value) { @media all {} } @supports (display: grid) { @media all {} } @layer 層 { .親 { @container (width > 0px) { @scope { @media all {} } } } } @function --幅() { @media all { result: 1px; } }";
        let source = sheet(text);
        let other = sheet(text);
        assert_eq!(source.rules().materialized_rules().len(), 5);
        let outer = media_at(&source, &[0]);
        let inner = media_at(&source, &[0, 0]);
        let unsupported = media_at(&source, &[1, 0]);
        let supported = media_at(&source, &[2, 0]);
        let scoped = media_at(&source, &[3, 0, 0, 0, 0]);
        let function = media_at(&source, &[4, 0]);
        let mut media_states =
            std::collections::HashMap::<u64, crate::css::style_sheet::NativeMediaEvaluationState>::new();
        let mut evaluate = |sheet: &NativeStyleSheet, width| {
            let media_state = media_states.entry(sheet.identity()).or_default();
            with_media_width(width, |environment| {
                sheet
                    .evaluate_media_queries(environment, media_state, &mut |_, _| {})
                    .any_changed
            })
        };
        assert!(!evaluate(&source, 50.0));
        assert!(!outer.matches());
        assert!(!inner.matches());
        assert!(!unsupported.matches());
        assert!(supported.matches());
        assert!(scoped.matches());
        assert!(function.matches());
        assert!(evaluate(&source, 150.0));
        assert!(outer.matches());
        assert!(!inner.matches());
        assert!(!evaluate(&source, 150.0));
        assert!(evaluate(&source, 250.0));
        assert!(inner.matches());
        assert!(evaluate(&source, 50.0));
        assert!(!outer.matches());
        // An inactive ancestor leaves its descendants' last evaluation untouched.
        assert!(inner.matches());
        assert!(!evaluate(&other, 250.0));
        assert!(!outer.matches());
        assert!(evaluate(&source, 250.0));
        assert!(!evaluate(&source, 250.0));
    }

    #[test]
    fn media_changes_publish_only_affected_subtrees_with_document_local_baselines() {
        let source = sheet(".unaffected {} @media (min-width: 100px) { .affected {} }");
        let unaffected = source.rules().rule_at(0).identity;
        let affected = source.rules().rule_at(1).children.as_ref().unwrap().rule_at(0).identity;
        let mut first = crate::css::style_sheet::NativeMediaEvaluationState::default();
        let mut second = crate::css::style_sheet::NativeMediaEvaluationState::default();
        with_media_width(150.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut first, &mut |_, _| {})
                    .any_changed
            );
        });
        with_media_width(50.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut second, &mut |_, _| {})
                    .any_changed
            );
            source.visit_conditions(environment, true, &mut |_, _| {});
        });
        with_media_width(150.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut first, &mut |_, _| panic!(
                        "unchanged document published conditions"
                    ))
                    .any_changed
            );
        });
        for (state, width, expected) in [(&mut first, 50.0, false), (&mut second, 150.0, true)] {
            let mut published = Vec::new();
            with_media_width(width, |environment| {
                let result = source.evaluate_media_queries(environment, state, &mut |identity, holds| {
                    published.push((identity, holds))
                });
                assert!(result.any_changed);
                assert!(!result.sheet_changed);
                assert!(!result.style_cache_inputs_changed);
                assert!(!result.layer_order_inputs_changed);
                assert!(!result.font_rule_inputs_changed);
            });
            assert!(!published.iter().any(|(identity, _)| *identity == unaffected));
            assert!(published.contains(&(affected, expected)));
        }
    }

    #[test]
    fn media_changes_report_font_and_layer_inputs() {
        let source = sheet(
            "@media (min-width: 100px) { @font-face { font-family: Test; src: url(test.woff); } @layer gated { .target {} } }",
        );
        let mut state = crate::css::style_sheet::NativeMediaEvaluationState::default();
        with_media_width(50.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut state, &mut |_, _| {})
                    .any_changed
            );
        });
        for width in [150.0, 50.0] {
            with_media_width(width, |environment| {
                let result = source.evaluate_media_queries(environment, &mut state, &mut |_, _| {});
                assert!(result.any_changed);
                assert!(result.style_cache_inputs_changed);
                assert!(result.layer_order_inputs_changed);
                assert!(result.font_rule_inputs_changed);
            });
        }
    }

    #[test]
    fn newly_loaded_imports_publish_conditions_on_the_next_media_evaluation() {
        let source = sheet("@import 'child.css';");
        let child = sheet("@media (min-width: 100px) { .target {} }");
        let target = child.rules().rule_at(0).children.as_ref().unwrap().rule_at(0).identity;
        let mut state = crate::css::style_sheet::NativeMediaEvaluationState::default();
        with_media_width(50.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut state, &mut |_, _| {})
                    .any_changed
            );
        });
        unsafe { rust_style_sheet_set_import(&source, source.rules().rule_at(0).identity, Rc::as_ptr(&child)) };
        with_media_width(150.0, |environment| {
            let mut published = Vec::new();
            let result = source.evaluate_media_queries(environment, &mut state, &mut |identity, holds| {
                published.push((identity, holds))
            });
            assert!(result.any_changed);
            assert!(published.contains(&(target, true)));
        });
    }

    #[test]
    fn empty_import_media_changes_invalidate_layer_order() {
        let source = sheet("@import 'child.css' layer(late) (min-width: 100px);");
        let child = NativeStyleSheet::new(
            crate::css::rule::NativeRuleList::new(Vec::new(), None),
            media_at(&source, &[0]),
        );
        unsafe { rust_style_sheet_set_import(&source, source.rules().rule_at(0).identity, Rc::as_ptr(&child)) };
        let mut state = crate::css::style_sheet::NativeMediaEvaluationState::default();
        with_media_width(50.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut state, &mut |_, _| {})
                    .any_changed
            );
        });
        for width in [150.0, 50.0] {
            with_media_width(width, |environment| {
                let result = source.evaluate_media_queries(environment, &mut state, &mut |_, _| {});
                assert!(result.any_changed);
                assert!(result.layer_order_inputs_changed);
            });
        }
    }

    #[test]
    fn media_evaluation_visits_every_import_and_propagates_changes() {
        let source = sheet("@import 'one.css'; @layer 層; @import 'two.css'; @media all {}");
        assert_eq!(source.rules().materialized_rules().len(), 4);
        let children = [
            sheet("@media (min-width: 100px) {}"),
            sheet("@media (min-width: 100px) {}"),
        ];
        let imports = [source.rules().rule_at(0).identity, source.rules().rule_at(2).identity];
        for (identity, child) in imports.iter().zip(&children) {
            unsafe { rust_style_sheet_set_import(&source, *identity, Rc::as_ptr(child)) };
        }
        let mut media_state = crate::css::style_sheet::NativeMediaEvaluationState::default();
        let media = children.each_ref().map(|child| media_at(child, &[0]));
        with_media_width(50.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut media_state, &mut |_, _| {})
                    .any_changed
            )
        });
        assert!(media.iter().all(|list| !list.matches()));
        with_media_width(150.0, |environment| {
            assert!(
                source
                    .evaluate_media_queries(environment, &mut media_state, &mut |_, _| {})
                    .any_changed
            );
            // A change in the first imported sheet must not skip evaluation of the second.
            assert!(media.iter().all(MediaList::matches));
            let mut visited = Vec::new();
            source.visit_conditions(environment, true, &mut |identity, _| visited.push(identity));
            assert_eq!(
                visited,
                [
                    imports[0],
                    children[0].rules().rule_at(0).identity,
                    source.rules().rule_at(1).identity,
                    imports[1],
                    children[1].rules().rule_at(0).identity,
                    source.rules().rule_at(3).identity
                ]
            );
        });
    }

    #[test]
    fn conditions_visit_inactive_descendants_and_import_gates() {
        let source = sheet(
            "@import 'one.css' supports(display: grid) all; @import 'two.css' supports(unknown: value); @import 'three.css' not all; @media (min-width: 100px) { .a {} @supports (unknown: value) { .b {} } @container (width > 999px) { .c {} } }",
        );
        assert_eq!(source.rules().materialized_rules().len(), 4);
        let imports = [0, 1, 2].map(|index| source.rules().rule_at(index).identity);
        let children = std::array::from_fn::<_, 3, _>(|_| sheet(".imported {}"));
        let child_rules = children.each_ref().map(|child| child.rules().rule_at(0).identity);
        for (identity, child) in imports.iter().zip(&children) {
            unsafe { rust_style_sheet_set_import(&source, *identity, Rc::as_ptr(child)) };
        }
        let imported_media = media_at(&source, &[0]);
        let outer = media_at(&source, &[3]);
        let visit = |holds, width| {
            with_media_width(width, |environment| {
                let mut conditions = Vec::new();
                let mut import_conditions = Vec::new();
                let mut import_identities = Vec::new();
                source.visit_conditions(environment, holds, &mut |identity, holds| {
                    if let Some(index) = child_rules.iter().position(|&child| child == identity) {
                        import_conditions.push(holds);
                        import_identities.push(imports[index]);
                    } else {
                        conditions.push(holds);
                    }
                });
                assert_eq!(import_identities, imports);
                (conditions, import_conditions)
            })
        };
        assert_eq!(visit(true, 150.0).1, [false, false, false]);
        with_media_width(150.0, |environment| assert!(imported_media.evaluate(environment)));
        let (conditions, gates) = visit(true, 150.0);
        assert_eq!(conditions, [true, true, true, true, true, true, false, true, true]);
        assert_eq!(gates, [true, false, false]);
        let mut media_state = crate::css::style_sheet::NativeMediaEvaluationState::default();
        // Condition publication does not establish the first media-change detection baseline.
        with_media_width(50.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut media_state, &mut |_, _| {})
                    .any_changed
            )
        });
        assert_eq!(
            visit(true, 50.0).0,
            [true, true, true, true, false, false, false, false, false]
        );
        visit(true, 150.0);
        let (conditions, gates) = visit(false, 50.0);
        assert_eq!(conditions, [false; 9]);
        assert_eq!(gates, [false; 3]);
        // Publication in another environment must not change the invalidation baseline.
        assert!(outer.matches());
        with_media_width(50.0, |environment| {
            assert!(
                !source
                    .evaluate_media_queries(environment, &mut media_state, &mut |_, _| {})
                    .any_changed
            )
        });
    }

    #[test]
    fn layer_names_preserve_identity_qualification_and_postorder() {
        let text = "@import 'one.css' layer(入口); @import 'two.css' layer; @import 'off.css' layer(無効) supports(unknown: value); @layer 外 { @layer 内, 次; @layer {} @media not all { @layer 隠; } @supports (unknown: value) { @layer 無; } @container (width > 999px) { @layer 有; } } @layer 終;";
        let source = sheet(text);
        let other = sheet(text);
        assert_eq!(source.rules().materialized_rules().len(), 5);
        let imported = source.rules().rule_at(1);
        let outer = source.rules().rule_at(3);
        let anonymous = outer.children.as_ref().unwrap().rule_at(1);
        let name = |rule: &NativeRule| RuleRef::Materialized(rule).internal_layer_name().unwrap().into_owned();
        let imported_name = name(&imported);
        let anonymous_name = name(&anonymous);
        assert_ne!(imported_name, anonymous_name);
        assert_ne!(imported_name, name(&other.rules().rule_at(1)));
        assert!(std::ptr::eq(
            RuleRef::Materialized(&outer).layer_names().unwrap(),
            RuleRef::Materialized(&other.rules().rule_at(3)).layer_names().unwrap()
        ));
        assert_eq!(name(&outer), "外".encode_utf16().collect::<Vec<_>>());
        assert!(
            RuleRef::Materialized(&source.rules().rule_at(4))
                .internal_layer_name()
                .is_none()
        );
        let child = sheet("@layer 子;");
        for index in [0, 1] {
            unsafe { rust_style_sheet_set_import(&source, source.rules().rule_at(index).identity, Rc::as_ptr(&child)) };
        }
        let mut names = Vec::new();
        source.visit_layer_names(&mut "根".encode_utf16().collect(), &mut |name| {
            names.push(name.to_vec())
        });
        let qualified =
            |prefix: &str, name: &[u16]| prefix.encode_utf16().chain(name.iter().copied()).collect::<Vec<_>>();
        let mut imported_child_name = qualified("根.", &imported_name);
        imported_child_name.extend(".子".encode_utf16());
        assert_eq!(
            names,
            [
                qualified("根.入口.子", &[]),
                qualified("根.入口", &[]),
                imported_child_name,
                qualified("根.", &imported_name),
                qualified("根.外.内", &[]),
                qualified("根.外.次", &[]),
                qualified("根.外.", &anonymous_name),
                qualified("根.外.有", &[]),
                qualified("根.外", &[]),
                qualified("根.終", &[])
            ]
        );
        crate::css::rule::rust_rule_list_remove(source.rules(), 0);
        assert_eq!(name(&source.rules().rule_at(0)), imported_name);
        crate::css::rule::rust_rule_list_clear(source.rules());
        assert_eq!(name(&anonymous), anonymous_name);
    }

    #[test]
    fn native_binding_caches_follow_selector_mutation_without_rebinding_scope_limits() {
        let source = sheet(".親 { & > .子 {} @scope (& .根) to (.限) {} }");
        let parent = source.rules().rule_at(0);
        let children = parent.children.as_ref().unwrap();
        let child = children.rule_at(0);
        let scope = children.rule_at(1);
        unsafe {
            let matching = child.matching_selectors();
            let start = scope.scope_start_selectors().unwrap();
            let end = scope.scope_end_selectors().unwrap();
            assert!(Rc::ptr_eq(&matching, &child.matching_selectors()));
            assert!(Rc::ptr_eq(&start, &scope.scope_start_selectors().unwrap()));
            assert!(Rc::ptr_eq(&end, &scope.scope_end_selectors().unwrap()));
            let units: Vec<_> = ".新".encode_utf16().collect();
            assert!(parent.set_selector_text(crate::css::css_tokenizer::TokenizerInput::Utf16(&units), None));
            assert!(!Rc::ptr_eq(&matching, &child.matching_selectors()));
            assert!(!Rc::ptr_eq(&start, &scope.scope_start_selectors().unwrap()));
            assert!(Rc::ptr_eq(&end, &scope.scope_end_selectors().unwrap()));
        }
    }

    #[test]
    fn ancestor_lifetimes_follow_detachment_and_reparenting() {
        use crate::css::rule::{rust_rule_list_clear, rust_rule_list_insert};
        fn ancestors(rule: &NativeRule) -> Vec<Rc<NativeRule>> {
            let mut result = Vec::new();
            let mut parent = rule.parent.borrow().upgrade();
            while let Some(ancestor) = parent {
                parent = ancestor.parent.borrow().upgrade();
                result.push(ancestor);
            }
            result
        }
        let source = sheet("@layer 外 { @media all { @scope (.根) { .葉 { width: 13px } } } } @media print {}");
        let roots = source.rules();
        let leaf = {
            let layer = roots.rule_at(0);
            let media = layer.children.as_ref().unwrap().rule_at(0);
            let scope = media.children.as_ref().unwrap().rule_at(0);
            scope.children.as_ref().unwrap().rule_at(0)
        };
        let destination = roots.rule_at(1);
        let retained = ancestors(&leaf);
        assert!(retained.iter().map(|rule| rule.rule_type).eq([
            NativeRuleType::Scope,
            NativeRuleType::Media,
            NativeRuleType::LayerBlock,
        ]));
        rust_rule_list_clear(roots);
        assert_eq!(ancestors(&leaf).len(), 3);
        drop(retained);
        assert!(ancestors(&leaf).is_empty());
        let children = destination.children.as_ref().unwrap();
        unsafe { rust_rule_list_insert(children, 0, Rc::as_ptr(&leaf)) };
        let reparented = ancestors(&leaf);
        assert_eq!(reparented.len(), 1);
        assert_eq!(reparented[0].identity, destination.identity);
        rust_rule_list_clear(children);
        assert!(ancestors(&leaf).is_empty());
    }

    #[test]
    fn cssom_access_only_materializes_requested_rules_and_preserves_detached_identities() {
        use crate::css::rule::{RULE_OWNER_ALLOCATIONS, rust_rule_list_clear, rust_rule_list_identity_at};

        let source = sheet(
            "@media all { .first { color: red; } .second { color: blue; } }
            .third { color: green; }
            @keyframes motion { from { opacity: 0; } to { opacity: 1; } }",
        );
        let list = source.rules();
        let before = RULE_OWNER_ALLOCATIONS.get();
        let identity = rust_rule_list_identity_at(list, 2);
        let mut path = Vec::new();
        assert!(list.find_path(identity, &mut path));
        assert_eq!(path, [2]);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before);

        let media = list.rule_at(0);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before + 1);
        let children = media.children.as_ref().unwrap();
        let second = children.rule_at(1);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before + 2);
        assert!(Rc::ptr_eq(&second, &children.rule_at(1)));
        assert_eq!(second.parent.borrow().upgrade().unwrap().identity, media.identity);
        path.clear();
        assert!(list.find_path(second.identity, &mut path));
        assert_eq!(path, [0, 1]);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before + 2);

        let keyframes = list.rule_at(2);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before + 3);
        let frames = keyframes.children.as_ref().unwrap();
        let frame = frames.rule_at(1);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before + 4);
        assert!(frames.rules.borrow().is_none());
        assert_eq!(frames.exposed_rules.borrow().len(), 1);
        assert!(list.rules.borrow().is_none());
        assert!(children.rules.borrow().is_none());

        rust_rule_list_clear(list);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before + 4);
        assert!(media.parent.borrow().upgrade().is_none());
        assert!(keyframes.parent.borrow().upgrade().is_none());
        assert_eq!(second.parent.borrow().upgrade().unwrap().identity, media.identity);
        assert_eq!(frame.parent.borrow().upgrade().unwrap().identity, keyframes.identity);
    }

    #[test]
    fn replacement_keeps_new_rules_shared_and_detaches_only_exposed_old_rules() {
        use crate::css::rule::{
            RULE_OWNER_ALLOCATIONS, rust_rule_list_identity_at, rust_rule_list_remove_imports, rust_rule_list_replace,
        };

        let old = sheet("@media all { .old { color: red; } .unread { color: blue; } } .other { width: 1px; }");
        let media = old.rules().rule_at(0);
        let child = media.children.as_ref().unwrap().rule_at(0);
        let replacement = sheet("@media all { .new { color: green; } } .last { width: 2px; }");
        let expected_identity = rust_rule_list_identity_at(replacement.rules(), 0);
        let before = RULE_OWNER_ALLOCATIONS.get();
        rust_rule_list_remove_imports(replacement.rules());
        rust_rule_list_replace(old.rules(), replacement.rules());
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before);
        assert!(old.rules().rules.borrow().is_none());
        assert!(replacement.rules().rules.borrow().is_none());
        assert!(media.parent.borrow().upgrade().is_none());
        assert_eq!(child.parent.borrow().upgrade().unwrap().identity, media.identity);
        let new_media = old.rules().rule_at(0);
        assert_eq!(new_media.identity, expected_identity);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), before + 1);
        assert!(new_media.children.as_ref().unwrap().rules.borrow().is_none());
    }

    #[test]
    fn shared_rule_traversal_preserves_identities_without_materializing_owners() {
        use crate::css::rule::{RuleRef, rust_rule_list_count, rust_rule_list_visit_images};
        use std::ops::ControlFlow;

        fn collect(rule: RuleRef<'_>, result: &mut Vec<(u64, u16)>) {
            result.push((rule.identity(), rule.rule_type() as u16));
            let _ = rule.visit_children(&mut |child| {
                collect(child, result);
                ControlFlow::Continue(())
            });
        }
        fn identities(list: &NativeRuleList) -> Vec<(u64, u16)> {
            let mut result = Vec::new();
            let _ = list.visit_rules(&mut |rule| {
                collect(rule, &mut result);
                ControlFlow::Continue(())
            });
            result
        }
        unsafe extern "C" fn image(context: *mut c_void, _: *const c_void) {
            unsafe { *context.cast::<usize>() += 1 };
        }

        let source = sheet(
            "@namespace 色 url(urn:color); .対象 { background-image: url(image.png);
            @media all { color: red; & > .子 { width: 1px; } } }
            @keyframes 動き { from { opacity: 0; } to { opacity: 1; } }
            @page { margin: 1px; @top-left { content: '頁'; } }
            @function --幅() { @media all { result: 1px; } }",
        );
        let rules = source.rules();
        assert_eq!(rust_rule_list_count(rules), 5);
        let before = identities(rules);
        let unique: std::collections::HashSet<_> = before.iter().map(|(identity, _)| *identity).collect();
        assert_eq!(unique.len(), before.len());
        let mut namespaces = 0;
        rules.for_each_namespace(|_| namespaces += 1);
        assert_eq!(namespaces, 1);
        let mut images = 0_usize;
        unsafe { rust_rule_list_visit_images(rules, (&raw mut images).cast(), image) };
        assert_eq!(images, 1);
        let environment = FfiMediaEnvironment {
            values: std::ptr::null(),
            value_count: 0,
            length_resolution_context: std::ptr::null(),
        };
        let mut media_state = crate::css::style_sheet::NativeMediaEvaluationState::default();
        assert!(
            !source
                .evaluate_media_queries(unsafe { environment.borrow() }, &mut media_state, &mut |_, _| {})
                .any_changed
        );
        assert!(rules.rules.borrow().is_none());

        rules.materialized_rules();
        assert_eq!(identities(rules), before);
        fn check_media(rule: RuleRef<'_>) {
            if let RuleRef::Materialized(rule) = rule
                && let RulePayload::Media { list } = &rule.payload
            {
                assert!(list.matches());
            }
            let _ = rule.visit_children(&mut |child| {
                check_media(child);
                ControlFlow::Continue(())
            });
        }
        let _ = rules.visit_rules(&mut |rule| {
            check_media(rule);
            ControlFlow::Continue(())
        });
        assert!(
            !source
                .evaluate_media_queries(unsafe { environment.borrow() }, &mut media_state, &mut |_, _| {})
                .any_changed
        );
    }

    #[test]
    fn compilation_publishes_shared_rules_without_allocating_mutable_owners() {
        use crate::css::declaration_block::DECLARATION_OWNER_ALLOCATIONS;
        use crate::css::rule::RULE_OWNER_ALLOCATIONS;
        use crate::css::style::StyleEngine;
        use crate::css::style::memory::DeviceClass;
        use crate::css::style::program::{CascadeOrigin, StyleSheetObjectID};
        fn downloaded_sheet(source: &str) -> Rc<NativeStyleSheet> {
            let units: Vec<_> = source.encode_utf16().collect();
            let parsed = std::thread::spawn(move || {
                let context: ParseContext = unsafe { std::mem::zeroed() };
                let parsed = unsafe {
                    parse_shared_stylesheet(crate::css::css_tokenizer::TokenizerInput::Utf16(&units), &context)
                };
                assert_eq!(RULE_OWNER_ALLOCATIONS.get(), 0);
                assert_eq!(DECLARATION_OWNER_ALLOCATIONS.get(), 0);
                assert_eq!(crate::css::descriptor_block::DESCRIPTOR_OWNER_ALLOCATIONS.get(), 0);
                assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
                parsed
            })
            .join()
            .unwrap();
            let rules = NativeRuleList::from_parsed(parsed);
            let media = MediaList::new(Default::default());
            NativeStyleSheet::new(rules, media)
        }
        unsafe extern "C" fn import(_: *const c_void, _: u64, sheet: &NativeStyleSheet) -> *const c_void {
            std::ptr::from_ref(sheet).cast()
        }
        unsafe extern "C" fn root(_: *const c_void) -> u32 {
            13
        }
        unsafe extern "C" fn visit(
            context: *const c_void,
            _: *const c_void,
            _: u64,
            _: NativeRuleType,
            _: &NativeCompilationContext,
            result: NativeCompilationResult,
        ) -> bool {
            if result.rule_id != 0 {
                unsafe { &*context.cast::<std::cell::Cell<usize>>() }.update(|count| count + 1);
            }
            true
        }

        let rule_owners = RULE_OWNER_ALLOCATIONS.get();
        let declaration_owners = DECLARATION_OWNER_ALLOCATIONS.get();
        let descriptor_owners = crate::css::descriptor_block::DESCRIPTOR_OWNER_ALLOCATIONS.get();
        let source = downloaded_sheet(
            "@import url(子😀.css) layer(子) all;
            @layer 色 { :nth-child(2) { color: red; --幅: 19px; width: var(--幅);
                @media all { & > :where(:root) { width: 1px; } }
                @scope (&) to (:scope > :where(:root)) { color: blue; }
            } }
            @keyframes 色 { from { opacity: 0; } to { opacity: 1; } }
            @property --色 { syntax: '<color>'; inherits: false; initial-value: red; }
            @counter-style 色 { system: cyclic; symbols: '色'; }
            @font-feature-values 色 { @styleset { 色: 1; } }
            @font-face { font-family: 色; src: url(字体😀.woff2); }
            @function --色() { result: 1px; }",
        );
        let parsed_child = downloaded_sheet(":root { --高さ: 2px; height: var(--高さ); }");
        let mut loaded_child = None;
        let mut import_identity = 0;
        let _ = source.rules().visit_rules(&mut |rule| {
            assert!(rule.rule_type() == NativeRuleType::Import);
            import_identity = rule.identity();
            let media = unsafe {
                Box::from_raw(crate::css::rule::read::rust_rule_view_import_media(&NativeRuleView {
                    rule,
                    containers: &[],
                }))
            };
            let child = unsafe {
                Rc::from_raw(rust_style_sheet_create(
                    std::ptr::from_ref(parsed_child.rules()),
                    &media,
                ))
            };
            unsafe {
                rust_style_sheet_set_import(&source, rule.identity(), Rc::as_ptr(&child));
            }
            loaded_child = Some(child);
            std::ops::ControlFlow::Break(())
        });
        drop(parsed_child);
        let child = loaded_child.unwrap();
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let compiled_sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let compiled = std::cell::Cell::new(0_usize);
        let callbacks = NativeCompilationCallbacks {
            context: (&raw const compiled).cast(),
            import_source: import,
            implicit_scope_root: root,
            visit_rule: visit,
        };
        let publication = NativeStylePublication {
            engine: (&raw mut engine).cast(),
            sheet: compiled_sheet.0 + 1,
            before_rule: 0,
        };
        let environment = FfiMediaEnvironment {
            values: std::ptr::null(),
            value_count: 0,
            length_resolution_context: std::ptr::null(),
        };
        let borrowed_environment = unsafe { environment.borrow() };
        let mut media_state = crate::css::style_sheet::NativeMediaEvaluationState::default();
        assert!(
            !source
                .evaluate_media_queries(borrowed_environment, &mut media_state, &mut |_, _| {})
                .any_changed
        );
        assert!(
            !source
                .evaluate_media_queries(borrowed_environment, &mut media_state, &mut |_, _| {})
                .any_changed
        );
        let mut layers = Vec::<Vec<u16>>::new();
        source.visit_layer_names(&mut Vec::new(), &mut |name| layers.push(name.to_vec()));
        assert_eq!(
            layers,
            [
                "子".encode_utf16().collect::<Vec<_>>(),
                "色".encode_utf16().collect::<Vec<_>>()
            ]
        );
        let mut conditions = 0_usize;
        source.visit_conditions(borrowed_environment, true, &mut |_, matches| {
            assert!(matches);
            conditions += 1;
        });
        assert!(conditions > 9);
        unsafe {
            rust_style_sheet_compile(
                &source,
                0,
                Rc::as_ptr(&source).cast(),
                environment,
                &callbacks,
                &publication,
            );
        }
        assert_eq!(compiled.get(), 9);
        source.publish_conditions(&mut engine, borrowed_environment);
        unsafe extern "C" fn prepare(_: *mut c_void) {}
        let sheets = [Rc::as_ptr(&source)];
        assert!(unsafe {
            crate::css::style_sheet::rust_style_sheet_publish_layer_order(
                sheets.as_ptr(),
                sheets.len(),
                (&raw mut engine).cast(),
                0,
                false,
                std::ptr::null_mut(),
                prepare,
            )
        });
        let next = crate::css::rule::mutation::successor(&source, import_identity, |identity| unsafe {
            crate::css::style::bridge::style_engine_native_rule_id((&raw const engine).cast(), identity)
        });
        assert_eq!(next, 2);
        let mut imported_engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let imported_sheet = imported_engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        compiled.set(0);
        let imported_publication = NativeStylePublication {
            engine: (&raw mut imported_engine).cast(),
            sheet: imported_sheet.0 + 1,
            ..publication
        };
        unsafe {
            rust_style_sheet_compile(
                &source,
                import_identity,
                Rc::as_ptr(&source).cast(),
                environment,
                &callbacks,
                &imported_publication,
            );
        }
        assert_eq!(compiled.get(), 1);
        use crate::css::rule::FfiRuleTraversalOrder;
        use crate::css::rule::function::{
            CompiledFunction, rust_compiled_function_visit_declarations, rust_rule_view_compile_function,
        };
        use crate::css::rule::read::{
            NativeRuleView, rust_rule_view_keyframes, rust_rule_view_property, rust_rule_view_type,
        };
        use std::sync::Arc;
        #[derive(Default)]
        struct Definitions {
            functions: RefCell<Vec<Arc<CompiledFunction>>>,
            keyframes: std::cell::Cell<usize>,
            properties: std::cell::Cell<usize>,
            descriptors: std::cell::Cell<usize>,
            font_features: std::cell::Cell<usize>,
        }
        unsafe extern "C" fn frame(
            context: *const c_void,
            _: *const f64,
            count: usize,
            _: *const crate::css::declaration_block::DeclarationBlockData,
        ) {
            assert_eq!(count, 1);
            let definitions = unsafe { &*context.cast::<Definitions>() };
            definitions.keyframes.set(definitions.keyframes.get() + 1);
        }
        unsafe extern "C" fn definition(context: *const c_void, rule: &NativeRuleView<'_>, _: FfiUtf16View) {
            let definitions = unsafe { &*context.cast::<Definitions>() };
            match rust_rule_view_type(rule) {
                NativeRuleType::Function => definitions
                    .functions
                    .borrow_mut()
                    .push(unsafe { Arc::from_raw(rust_rule_view_compile_function(rule)) }),
                NativeRuleType::Keyframes => unsafe {
                    rust_rule_view_keyframes(rule, context, frame);
                },
                NativeRuleType::Property => {
                    assert!(!rust_rule_view_property(rule).is_null());
                    definitions.properties.set(definitions.properties.get() + 1);
                }
                NativeRuleType::CounterStyle | NativeRuleType::FontFace => {
                    let descriptors =
                        unsafe { Box::from_raw(crate::css::rule::read::rust_rule_view_descriptors(rule)) };
                    assert_eq!(descriptors.data().descriptors.len(), 2);
                    definitions.descriptors.set(definitions.descriptors.get() + 1);
                }
                NativeRuleType::FontFeatureValues => {
                    let data =
                        unsafe { Arc::from_raw(crate::css::rule::read::rust_rule_view_font_feature_values(rule)) };
                    assert_eq!(
                        crate::css::font_feature_values::rust_font_feature_values_data_family_count(&data),
                        1
                    );
                    definitions.font_features.set(definitions.font_features.get() + 1);
                }
                _ => {}
            }
        }
        let definitions = Definitions::default();
        unsafe {
            crate::css::style_sheet::rust_style_sheet_visit_effective_rule_data(
                &source,
                FfiRuleTraversalOrder::Preorder,
                (&raw const definitions).cast(),
                definition,
            );
        }
        assert_eq!(definitions.keyframes.get(), 2);
        assert_eq!(definitions.properties.get(), 1);
        assert_eq!(definitions.functions.borrow().len(), 1);
        assert_eq!(definitions.descriptors.get(), 2);
        assert_eq!(definitions.font_features.get(), 1);
        let font_definitions = Definitions::default();
        unsafe extern "C" fn font_definition(context: *const c_void, rule: &NativeRuleView<'_>) {
            unsafe { definition(context, rule, FfiUtf16View::default()) };
        }
        unsafe {
            crate::css::rule::read::rust_rule_list_visit_font_rules(
                source.rules(),
                (&raw const font_definitions).cast(),
                font_definition,
            );
        }
        assert_eq!(font_definitions.descriptors.get(), 1);
        assert_eq!(font_definitions.font_features.get(), 1);
        let _ = source.rules().visit_descendant_rules(&mut |rule| {
            if rule.rule_type() == NativeRuleType::FontFace {
                let descriptors = unsafe {
                    Box::from_raw(crate::css::rule::read::rust_rule_list_descriptor_snapshot(
                        source.rules(),
                        rule.identity(),
                    ))
                };
                assert_eq!(descriptors.data().descriptors.len(), 2);
            }
            std::ops::ControlFlow::Continue(())
        });
        assert!(source.rules().rules.borrow().is_none());
        assert!(child.rules().rules.borrow().is_none());
        unsafe extern "C" fn matches(_: *mut c_void, _: *const *const ContainerConditionsData, _: usize) -> bool {
            unreachable!()
        }
        unsafe extern "C" fn declaration(context: *mut c_void, name: FfiUtf16View, _: *const c_void) {
            assert_eq!(
                unsafe { std::slice::from_raw_parts(name.utf16, name.length) },
                "result".encode_utf16().collect::<Vec<_>>()
            );
            unsafe {
                *context.cast::<usize>() += 1;
            }
        }
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), rule_owners);
        assert_eq!(DECLARATION_OWNER_ALLOCATIONS.get(), declaration_owners);
        assert_eq!(
            crate::css::descriptor_block::DESCRIPTOR_OWNER_ALLOCATIONS.get(),
            descriptor_owners
        );

        // Exposing one nested style rule must not replace the shared compilation path
        // with a document-local graph for its siblings or descendants.
        let layer = source.rules().rule_at(1);
        let style = layer.children.as_ref().unwrap().rule_at(0);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), rule_owners + 2);
        assert!(source.rules().rules.borrow().is_none());
        assert!(layer.children.as_ref().unwrap().rules.borrow().is_none());
        assert!(style.children.as_ref().unwrap().rules.borrow().is_none());
        let mut exposed_engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let exposed_sheet = exposed_engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let exposed_publication = NativeStylePublication {
            engine: (&raw mut exposed_engine).cast(),
            sheet: exposed_sheet.0 + 1,
            ..publication
        };
        compiled.set(0);
        unsafe {
            rust_style_sheet_compile(
                &source,
                0,
                Rc::as_ptr(&source).cast(),
                environment,
                &callbacks,
                &exposed_publication,
            );
        }
        assert_eq!(compiled.get(), 9);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), rule_owners + 2);
        assert_eq!(DECLARATION_OWNER_ALLOCATIONS.get(), declaration_owners);
        assert_eq!(
            crate::css::descriptor_block::DESCRIPTOR_OWNER_ALLOCATIONS.get(),
            descriptor_owners
        );
        drop(style);
        drop(layer);
        drop(source);
        drop(child);
        let mut declarations = 0_usize;
        unsafe {
            rust_compiled_function_visit_declarations(
                &definitions.functions.borrow()[0],
                (&raw mut declarations).cast(),
                matches,
                declaration,
            );
        }
        assert_eq!(declarations, 1);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), rule_owners + 2);
        assert_eq!(DECLARATION_OWNER_ALLOCATIONS.get(), declaration_owners);
        assert_eq!(
            crate::css::descriptor_block::DESCRIPTOR_OWNER_ALLOCATIONS.get(),
            descriptor_owners
        );
    }

    #[test]
    fn traversal_binds_selectors_without_native_parents_or_caches() {
        use crate::css::selector_parser::RustBoundSelectorList;

        fn visit(rule: &NativeRule, inputs: &SelectorInputs, bound: &mut Vec<Rc<RustBoundSelectorList>>) {
            // The traversal must supply nesting even when no parent owner can be consulted.
            *rule.parent.borrow_mut() = Default::default();
            let selectors = unsafe { inputs.matching_selectors(RuleRef::Materialized(rule)) };
            if let Some(selectors) = &selectors {
                bound.push(selectors.clone());
            }
            let nested = unsafe { inputs.within(RuleRef::Materialized(rule), selectors, u32::MAX) };
            if let Some(children) = &rule.children {
                for child in children.materialized_rules().iter() {
                    visit(child, &nested, bound);
                }
            }
            assert!(rule.matching_selectors.borrow().is_none());
            assert!(rule.scope_end_selectors.borrow().is_none());
        }

        let sheet = sheet(
            ":nth-child(2) { @media all { & > :where(:root) { color: red; } }
                @scope (&) to (:scope > :where(:root)) { color: blue; > :where(:root) { color: green; } } }",
        );
        let mut bound = Vec::new();
        visit(
            &sheet.rules().materialized_rules()[0],
            &SelectorInputs::default(),
            &mut bound,
        );
        drop(sheet);
        let specificities: Vec<_> = bound
            .iter()
            .map(|list| list.selectors[0].specificity().classes)
            .collect();
        assert_eq!(specificities, [1, 1, 0, 0]);
        assert!(bound.iter().all(|list| {
            list.selectors
                .iter()
                .all(|selector| !crate::css::selector_operations::contains_nesting(selector))
        }));
    }

    #[test]
    fn namespace_parse_context_outlives_the_native_sheet_on_a_worker() {
        use crate::css::rule::{rust_rule_list_clear, rust_rule_list_namespace_context};
        use std::sync::Arc;
        let sheet = sheet("@namespace 色 url(urn:color); @namespace 色 url(urn:other);");
        let namespaces = unsafe { Arc::from_raw(rust_rule_list_namespace_context(sheet.rules())) };
        assert_eq!(namespaces.prefixes.len(), 1);
        rust_rule_list_clear(sheet.rules());
        drop(sheet);
        std::thread::spawn(move || {
            let context = ParseContext {
                declared_namespaces: Arc::as_ptr(&namespaces),
                // All remaining fields are booleans, integers, or nullable pointers.
                ..unsafe { std::mem::zeroed() }
            };
            for (source, expected) in [("色|item { color: red; }", 1), ("未知|item { color: red; }", 0)] {
                let units: Vec<_> = source.encode_utf16().collect();
                let view = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
                let parsed = unsafe { parse_shared_stylesheet(view, &context) };
                let rules = crate::css::rule::NativeRuleList::from_parsed(parsed);
                assert_eq!(rules.materialized_rules().len(), expected);
            }
        })
        .join()
        .unwrap();
    }

    #[test]
    fn native_cssom_insertion_validates_before_attaching_the_rule() {
        use crate::css::rule::{RuleInsertionResult, rust_rule_list_insert_css_rule};
        let target = sheet("@namespace 色 url(urn:color);");
        let source = sheet("@import url(a.css); @namespace 新 url(urn:new); div { color: red; }");
        let rules = source.rules().materialized_rules();
        let import = &rules[0];
        let namespace = &rules[1];
        let style = &rules[2];
        let insert = |rule: &Rc<NativeRule>, index, nested| unsafe {
            rust_rule_list_insert_css_rule(target.rules(), index, Rc::as_ptr(rule), nested)
        };
        assert_eq!(insert(style, 2, false), RuleInsertionResult::IndexSizeError);
        assert_eq!(insert(style, 0, false), RuleInsertionResult::HierarchyRequestError);
        assert_eq!(insert(import, 1, false), RuleInsertionResult::HierarchyRequestError);
        assert_eq!(insert(import, 0, true), RuleInsertionResult::HierarchyRequestError);
        assert_eq!(target.rules().materialized_rules().len(), 1);
        assert_eq!(insert(style, 1, false), RuleInsertionResult::Success);
        assert_eq!(insert(namespace, 1, false), RuleInsertionResult::InvalidStateError);
        assert_eq!(target.rules().materialized_rules().len(), 2);
        assert!(Rc::ptr_eq(&target.rules().materialized_rules()[1], style));
    }

    #[test]
    fn native_removal_validation_preserves_the_rule_list() {
        use crate::css::rule::{RuleRemovalResult, rust_rule_list_remove, rust_rule_list_validate_removal};
        let sheet = sheet("@namespace p url(urn:p); div {}");
        let rules = sheet.rules();
        assert_eq!(
            rust_rule_list_validate_removal(rules, 2),
            RuleRemovalResult::IndexSizeError
        );
        assert_eq!(
            rust_rule_list_validate_removal(rules, 0),
            RuleRemovalResult::InvalidStateError
        );
        assert_eq!(rules.materialized_rules().len(), 2);
        assert_eq!(rust_rule_list_validate_removal(rules, 1), RuleRemovalResult::Success);
        rust_rule_list_remove(rules, 1);
        assert_eq!(rust_rule_list_validate_removal(rules, 0), RuleRemovalResult::Success);
    }

    #[test]
    fn function_mutation_context_follows_native_membership() {
        use crate::css::rule::{rust_rule_containing_function, rust_rule_list_clear};
        let sheet = sheet("@function --f() { @media all { result: 1px; } }");
        let function = sheet.rules().materialized_rules()[0].clone();
        let body = function.children.as_ref().unwrap();
        let group = body
            .materialized_rules()
            .iter()
            .find(|rule| rule.rule_type == NativeRuleType::Media)
            .unwrap()
            .clone();
        let declarations = group.children.as_ref().unwrap().materialized_rules()[0].clone();
        assert_eq!(rust_rule_containing_function(&function), Rc::as_ptr(&function));
        assert_eq!(rust_rule_containing_function(&group), Rc::as_ptr(&function));
        assert_eq!(rust_rule_containing_function(&declarations), Rc::as_ptr(&function));
        rust_rule_list_clear(body);
        assert!(rust_rule_containing_function(&group).is_null());
        assert!(rust_rule_containing_function(&declarations).is_null());
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    struct CompiledRule {
        identity: u64,
        source: *const c_void,
        layer: Vec<u16>,
        scopes: Vec<(u64, u32)>,
        container_count: usize,
        conditions_hold: bool,
        in_a_layer: bool,
        gated_by_container_query: bool,
    }

    fn collect(sheet: &NativeStyleSheet, identity: u64, purpose: NativeCompilationPurpose) -> Vec<CompiledRule> {
        struct Collection {
            rules: RefCell<Vec<CompiledRule>>,
        }
        unsafe extern "C" fn import(_: *const c_void, _: u64, sheet: &NativeStyleSheet) -> *const c_void {
            (sheet as *const NativeStyleSheet).cast()
        }
        unsafe extern "C" fn root(_: *const c_void) -> u32 {
            13
        }
        unsafe extern "C" fn visit(
            data: *const c_void,
            source: *const c_void,
            identity: u64,
            rule_type: NativeRuleType,
            context: &NativeCompilationContext,
            _: NativeCompilationResult,
        ) -> bool {
            let collection = unsafe { &*data.cast::<Collection>() };
            if rule_type == NativeRuleType::Style {
                let layer = unsafe { std::slice::from_raw_parts(context.layer_name.utf16, context.layer_name.length) };
                let scopes = unsafe { std::slice::from_raw_parts(context.scopes, context.scope_count) };
                collection.rules.borrow_mut().push(CompiledRule {
                    identity,
                    source,
                    layer: layer.to_vec(),
                    scopes: scopes
                        .iter()
                        .map(|scope| (scope.identity, scope.implicit_root))
                        .collect(),
                    container_count: context.container_count,
                    conditions_hold: context.conditions_hold,
                    in_a_layer: context.in_a_layer,
                    gated_by_container_query: context.gated_by_container_query,
                });
            }
            true
        }
        let collection = Collection {
            rules: RefCell::default(),
        };
        let callbacks = NativeCompilationCallbacks {
            context: (&raw const collection).cast(),
            import_source: import,
            implicit_scope_root: root,
            visit_rule: visit,
        };
        let environment = FfiMediaEnvironment {
            values: std::ptr::null(),
            value_count: 0,
            length_resolution_context: std::ptr::null(),
        };
        unsafe {
            rust_style_sheet_visit_compilation(
                sheet,
                identity,
                purpose,
                (sheet as *const NativeStyleSheet).cast(),
                environment,
                &callbacks,
            );
        }
        collection.rules.into_inner()
    }

    #[test]
    fn individual_rules_inherit_the_same_native_import_context_as_whole_sheets() {
        let root = sheet("@import 'child.css' layer(外) scope((.outer));");
        let child = sheet(
            "@layer 内 { @scope (.inner) { @container (width > 1px) {
            .x { color: red; } @media not all { .y { color: blue; } }
        } } }",
        );
        let import = root.rules().materialized_rules()[0].identity;
        unsafe { rust_style_sheet_set_import(&root, import, Rc::as_ptr(&child)) };
        let rules = collect(&root, 0, NativeCompilationPurpose::Rules);
        assert_eq!(rules.len(), 2);
        for rule in &rules {
            assert_eq!(rule.source, Rc::as_ptr(&child).cast());
            assert_eq!(rule.layer, "外.内".encode_utf16().collect::<Vec<_>>());
            assert_eq!(rule.scopes.len(), 2);
            assert!(rule.scopes.iter().all(|scope| scope.1 == 13));
            assert_eq!(rule.container_count, 1);
            assert!(rule.in_a_layer && rule.gated_by_container_query);
            assert_eq!(
                collect(&root, rule.identity, NativeCompilationPurpose::Rules),
                vec![rule.clone()]
            );
        }
        assert!(rules[0].conditions_hold);
        assert!(!rules[1].conditions_hold);
    }

    #[test]
    fn selector_replacement_does_not_change_media_match_state() {
        let sheet = sheet("@media all { @scope (.root) { .target { color: red; } } }");
        let media = sheet.rules().materialized_rules()[0].clone();
        let scope = media.children.as_ref().unwrap().materialized_rules()[0].clone();
        let target = scope.children.as_ref().unwrap().materialized_rules()[0].identity;
        let RulePayload::Media { list, .. } = &media.payload else {
            unreachable!()
        };
        assert!(!list.matches());
        let selectors = collect(&sheet, target, NativeCompilationPurpose::Selectors);
        assert_eq!(selectors.len(), 1);
        assert_eq!(selectors[0].scopes, vec![(scope.identity, 13)]);
        assert!(!list.matches());
        let rules = collect(&sheet, target, NativeCompilationPurpose::Rules);
        assert!(rules[0].conditions_hold);
        assert!(list.matches());
    }

    #[test]
    fn selector_traversal_preserves_descendant_media_state() {
        let sheet = sheet(".parent { @media all { & .nested { color: red; } } }");
        let parent = sheet.rules().materialized_rules()[0].clone();
        let media = parent.children.as_ref().unwrap().materialized_rules()[0].clone();
        let RulePayload::Media { list, .. } = &media.payload else {
            unreachable!()
        };
        assert!(!list.matches());
        let selectors = collect(&sheet, parent.identity, NativeCompilationPurpose::Selectors);
        assert_eq!(selectors.len(), 2);
        assert!(!list.matches());
        let rules = collect(&sheet, parent.identity, NativeCompilationPurpose::Rules);
        assert_eq!(rules.len(), 2);
        assert!(list.matches());
    }

    #[test]
    fn keyframe_children_share_declarations_and_track_native_membership() {
        use crate::css::declaration_block::rust_declaration_block_identity;
        use crate::css::rule::{
            rust_rule_declarations, rust_rule_list_clear, rust_rule_list_insert, rust_rule_list_remove,
        };
        let sheet = sheet("@keyframes 色 { from { opacity: 0.25; } to { opacity: 0.75; } }");
        let rule = sheet.rules().materialized_rules()[0].clone();
        let children = rule.children.as_ref().unwrap();
        let first = children.materialized_rules()[0].clone();
        assert_eq!(first.declaration_owner_identity(), Some(rule.identity));
        let native = unsafe { Box::from_raw(rust_rule_declarations(&first)) };
        let animation = unsafe { Box::from_raw(rust_rule_declarations(&children.rule_at(0))) };
        assert_eq!(
            rust_declaration_block_identity(&native),
            rust_declaration_block_identity(&animation)
        );
        rust_rule_list_remove(children, 0);
        assert_eq!(first.declaration_owner_identity(), None);
        assert_eq!(children.materialized_rules().len(), 1);
        unsafe { rust_rule_list_insert(children, 1, Rc::as_ptr(&first)) };
        assert_eq!(first.declaration_owner_identity(), Some(rule.identity));
        let reinserted = unsafe { Box::from_raw(rust_rule_declarations(&children.rule_at(1))) };
        assert_eq!(
            rust_declaration_block_identity(&native),
            rust_declaration_block_identity(&reinserted)
        );
        rust_rule_list_clear(children);
        assert_eq!(children.materialized_rules().len(), 0);
        assert_eq!(first.declaration_owner_identity(), None);
    }

    #[test]
    fn native_rule_identity_survives_tree_edits() {
        use crate::css::rule::{
            rust_rule_list_clear, rust_rule_list_insert, rust_rule_list_remove, rust_rule_list_replace,
        };
        let source = ".先頭 {} @media all { .兄弟 {} @supports (display: grid) { .対象 { width: 13px } } }";
        let first = sheet(source);
        let second = sheet(source);
        let rules = first.rules();
        let other = second.rules();
        let media = rules.materialized_rules()[1].clone();
        let children = media.children.as_ref().unwrap();
        let supports = children.materialized_rules()[1].clone();
        let descendants = supports.children.as_ref().unwrap();
        let target = descendants.materialized_rules()[0].clone();
        let path = |list: &NativeRuleList, identity| {
            let mut path = Vec::new();
            list.find_path(identity, &mut path);
            path
        };
        assert_eq!(path(rules, target.identity), [1, 1, 0]);
        assert!(path(other, target.identity).is_empty());
        assert!(path(rules, 0).is_empty());
        rust_rule_list_remove(rules, 0);
        assert_eq!(path(rules, target.identity), [0, 1, 0]);
        rust_rule_list_remove(children, 0);
        assert_eq!(path(rules, target.identity), [0, 0, 0]);
        rust_rule_list_remove(descendants, 0);
        assert!(path(rules, target.identity).is_empty());
        unsafe { rust_rule_list_insert(descendants, 0, Rc::as_ptr(&target)) };
        assert_eq!(path(rules, target.identity), [0, 0, 0]);
        rust_rule_list_clear(rules);
        assert!(path(rules, target.identity).is_empty());
        unsafe { rust_rule_list_insert(rules, 0, Rc::as_ptr(&media)) };
        assert_eq!(path(rules, target.identity), [0, 0, 0]);
        rust_rule_list_replace(rules, other);
        assert!(path(rules, target.identity).is_empty());
    }
}
