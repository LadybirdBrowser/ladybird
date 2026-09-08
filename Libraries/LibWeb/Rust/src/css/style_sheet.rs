/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::container_conditions::ContainerConditionsData;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::media_list::MediaList;
use crate::css::parser::query_parser::{FfiMediaEnvironment, MediaEnvironment};
use crate::css::rule::read::{NativeRuleView, RuleRef};
use crate::css::rule::{FfiRuleTraversalOrder, NativeRuleList, NativeRuleType};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_void;
use std::ops::ControlFlow;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_SHEET_IDENTITY: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy)]
#[repr(u8)]
#[cfg_attr(not(test), expect(dead_code, reason = "Flags are selected by CSSOM through FFI"))]
pub enum NativeStyleSheetFlag {
    Disabled = 1,
    Alternate = 2,
    OriginClean = 4,
    Constructed = 8,
    DisallowModification = 16,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum NativeStyleSheetMediaState {
    Unevaluated,
    NotMatched,
    Matched,
}

#[derive(Default)]
#[repr(C)]
pub struct NativeStyleSheetMediaEvaluation {
    pub sheet_changed: bool,
    pub any_changed: bool,
    pub style_cache_inputs_changed: bool,
    pub layer_order_inputs_changed: bool,
    pub font_rule_inputs_changed: bool,
}

#[derive(Default)]
pub struct NativeMediaEvaluationState {
    sheets: HashMap<u64, bool>,
    rules: HashMap<u64, bool>,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_evaluation_state_create() -> *mut NativeMediaEvaluationState {
    Box::into_raw(Box::default())
}

/// # Safety
/// The state must be an owned pointer returned by rust_media_evaluation_state_create.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_evaluation_state_free(state: *mut NativeMediaEvaluationState) {
    drop(unsafe { Box::from_raw(state) });
}

// Document-local state. The rule and media owners retain their immutable parsed data separately,
// so CSSOM edits detach that data without changing another sheet or the worker's parsed result.
pub struct NativeStyleSheet {
    identity: u64,
    rules: Rc<NativeRuleList>,
    media: MediaList,
    flags: Cell<u8>,
    media_state: Cell<NativeStyleSheetMediaState>,
    imports: RefCell<HashMap<u64, Rc<NativeStyleSheet>>>,
}

impl NativeStyleSheet {
    pub(crate) fn identity(&self) -> u64 {
        self.identity
    }

    pub(super) fn rules(&self) -> &NativeRuleList {
        &self.rules
    }

    pub(super) fn imported_sheet(&self, identity: u64) -> Option<Rc<Self>> {
        self.imports.borrow().get(&identity).cloned()
    }

    pub(crate) fn evaluate_media_queries(
        &self,
        environment: MediaEnvironment<'_>,
        state: &mut NativeMediaEvaluationState,
        publish: &mut impl FnMut(u64, bool),
    ) -> NativeStyleSheetMediaEvaluation {
        fn note_changed(rule: RuleRef<'_>, result: &mut NativeStyleSheetMediaEvaluation) {
            result.style_cache_inputs_changed |= !matches!(
                rule.rule_type(),
                NativeRuleType::Style
                    | NativeRuleType::Media
                    | NativeRuleType::Supports
                    | NativeRuleType::Scope
                    | NativeRuleType::LayerBlock
                    | NativeRuleType::LayerStatement
                    | NativeRuleType::NestedDeclarations
                    | NativeRuleType::Namespace
                    | NativeRuleType::Page
                    | NativeRuleType::Margin
            );
            result.layer_order_inputs_changed |= matches!(
                rule.rule_type(),
                NativeRuleType::LayerBlock | NativeRuleType::LayerStatement | NativeRuleType::Import
            );
            result.font_rule_inputs_changed |= matches!(
                rule.rule_type(),
                NativeRuleType::FontFace | NativeRuleType::FontFeatureValues | NativeRuleType::Import
            );
            let _ = rule.visit_children(&mut |child| {
                note_changed(child, result);
                ControlFlow::Continue(())
            });
        }
        struct Evaluation<'a, 'b, F> {
            environment: MediaEnvironment<'a>,
            previous: NativeMediaEvaluationState,
            state: &'b mut NativeMediaEvaluationState,
            result: NativeStyleSheetMediaEvaluation,
            publish: &'b mut F,
        }
        impl<F: FnMut(u64, bool)> Evaluation<'_, '_, F> {
            fn sheet(&mut self, sheet: &NativeStyleSheet, conditions_hold: bool, ancestor_changed: bool) -> bool {
                let matches = sheet.media.evaluate(self.environment);
                sheet.media_state.set(if matches {
                    NativeStyleSheetMediaState::Matched
                } else {
                    NativeStyleSheetMediaState::NotMatched
                });
                self.state.sheets.insert(sheet.identity(), matches);
                let changed = self
                    .previous
                    .sheets
                    .get(&sheet.identity())
                    .map_or(!self.previous.sheets.is_empty(), |previous| *previous != matches);
                self.result.any_changed |= changed;
                let _ = sheet.rules.visit_rules(&mut |rule| {
                    if matches {
                        self.rule(sheet, rule, conditions_hold, ancestor_changed || changed);
                    }
                    if changed {
                        note_changed(rule, &mut self.result);
                    }
                    ControlFlow::Continue(())
                });
                if changed && !ancestor_changed {
                    sheet.visit_conditions(self.environment, conditions_hold && matches, self.publish);
                }
                changed
            }

            fn rule(
                &mut self,
                sheet: &NativeStyleSheet,
                rule: RuleRef<'_>,
                conditions_hold: bool,
                ancestor_changed: bool,
            ) {
                let mut changed = false;
                let evaluate_children = match rule.rule_type() {
                    NativeRuleType::Media => {
                        let matches = rule.evaluate_media(self.environment);
                        self.state.rules.insert(rule.identity(), matches);
                        changed = self
                            .previous
                            .rules
                            .get(&rule.identity())
                            .is_some_and(|previous| *previous != matches);
                        self.result.any_changed |= changed;
                        matches
                    }
                    NativeRuleType::Supports => rule.condition_holds(self.environment),
                    NativeRuleType::Import => {
                        if let Some(imported) = sheet.imported_sheet(rule.identity())
                            && self.sheet(&imported, conditions_hold && rule.supports_hold(), ancestor_changed)
                        {
                            note_changed(rule, &mut self.result);
                        }
                        return;
                    }
                    NativeRuleType::Style
                    | NativeRuleType::Container
                    | NativeRuleType::Scope
                    | NativeRuleType::Function
                    | NativeRuleType::LayerBlock => true,
                    _ => false,
                };
                if evaluate_children {
                    let _ = rule.visit_children(&mut |child| {
                        self.rule(sheet, child, conditions_hold, ancestor_changed || changed);
                        ControlFlow::Continue(())
                    });
                }
                if changed {
                    let _ = rule.visit_children(&mut |child| {
                        note_changed(child, &mut self.result);
                        ControlFlow::Continue(())
                    });
                    if !ancestor_changed {
                        sheet.visit_rule_conditions(rule, self.environment, conditions_hold, self.publish);
                    }
                }
            }
        }
        // Invalidation baselines belong to the evaluating document. Condition publication can
        // temporarily evaluate a shared sheet in another document without changing its baseline.
        let previous = std::mem::take(state);
        let previous_sheet = previous.sheets.get(&self.identity()).copied();
        let mut evaluation = Evaluation {
            environment,
            previous,
            state,
            result: NativeStyleSheetMediaEvaluation::default(),
            publish,
        };
        evaluation.sheet(self, true, false);
        evaluation.result.sheet_changed = previous_sheet.is_some_and(|previous| previous != self.media.matches());
        evaluation.result
    }

    pub(crate) fn publish_conditions(
        &self,
        engine: &mut crate::css::style::StyleEngine,
        environment: MediaEnvironment<'_>,
    ) {
        self.visit_conditions(environment, true, &mut |identity, holds| {
            if let Some(rule) = engine.native_rule_id(identity) {
                crate::css::style::bridge::operations::set_rule_conditions_hold(engine, rule.0 + 1, holds);
            }
        });
    }

    pub(crate) fn visit_conditions(
        &self,
        environment: MediaEnvironment<'_>,
        conditions_hold: bool,
        visit_rule: &mut impl FnMut(u64, bool),
    ) {
        let _ = self.rules.visit_rules(&mut |rule| {
            self.visit_rule_conditions(rule, environment, conditions_hold, visit_rule);
            ControlFlow::Continue(())
        });
    }

    fn visit_rule_conditions(
        &self,
        rule: RuleRef<'_>,
        environment: MediaEnvironment<'_>,
        conditions_hold: bool,
        visit_rule: &mut impl FnMut(u64, bool),
    ) {
        visit_rule(rule.identity(), conditions_hold);
        let nested_conditions_hold = conditions_hold && rule.condition_holds(environment);
        if rule.rule_type() == NativeRuleType::Import {
            if let Some(imported) = self.imported_sheet(rule.identity()) {
                imported.visit_conditions(environment, nested_conditions_hold, visit_rule);
            }
        } else {
            let _ = rule.visit_children(&mut |child| {
                self.visit_rule_conditions(child, environment, nested_conditions_hold, visit_rule);
                ControlFlow::Continue(())
            });
        }
    }

    pub(crate) fn visit_layer_names(&self, prefix: &mut Vec<u16>, visit_name: &mut impl FnMut(&[u16])) {
        fn append_name(prefix: &mut Vec<u16>, name: &[u16]) {
            if !prefix.is_empty() {
                prefix.push(u16::from(b'.'));
            }
            prefix.extend_from_slice(name);
        }
        fn visit(
            sheet: &NativeStyleSheet,
            rule: RuleRef<'_>,
            prefix: &mut Vec<u16>,
            visit_name: &mut impl FnMut(&[u16]),
        ) {
            if !rule.cached_condition_holds() {
                return;
            }
            let prefix_length = prefix.len();
            if rule.rule_type() == NativeRuleType::LayerStatement {
                for name in &rule.layer_names().unwrap().names {
                    append_name(prefix, name.units());
                    visit_name(prefix);
                    prefix.truncate(prefix_length);
                }
                return;
            }
            let name = rule.internal_layer_name();
            if let Some(name) = &name {
                append_name(prefix, name);
            }
            if rule.rule_type() == NativeRuleType::Import {
                if let Some(imported) = sheet.imported_sheet(rule.identity()) {
                    imported.visit_layer_names(prefix, visit_name);
                }
            } else if rule.rule_type() != NativeRuleType::Function {
                let _ = rule.visit_children(&mut |child| {
                    visit(sheet, child, prefix, visit_name);
                    ControlFlow::Continue(())
                });
            }
            if name.is_some() {
                // https://drafts.csswg.org/css-cascade-5/#at-import
                // The layer is added to the layer order even if the import fails to load the stylesheet, but is
                // subject to any import conditions (just as if declared by an @layer rule wrapped in the appropriate
                // conditional group rules).
                visit_name(prefix);
            }
            prefix.truncate(prefix_length);
        }
        if self.media.matches() {
            let _ = self.rules.visit_rules(&mut |rule| {
                visit(self, rule, prefix, visit_name);
                ControlFlow::Continue(())
            });
        }
    }

    pub(crate) fn visit_effective_rule_data(
        &self,
        order: FfiRuleTraversalOrder,
        prefix: &[u16],
        visit: &mut impl FnMut(RuleRef<'_>, &[u16], &[Arc<ContainerConditionsData>]),
    ) {
        if !self.media.matches() {
            return;
        }
        fn walk(
            sheet: &NativeStyleSheet,
            rule: RuleRef<'_>,
            order: FfiRuleTraversalOrder,
            prefix: &[u16],
            containers: &mut Vec<Arc<ContainerConditionsData>>,
            visit: &mut impl FnMut(RuleRef<'_>, &[u16], &[Arc<ContainerConditionsData>]),
        ) {
            use crate::css::rule::NativeRuleType;
            if order == FfiRuleTraversalOrder::Preorder {
                visit(rule, prefix, containers);
            }
            let name = rule.internal_layer_name().map(|name| {
                let mut qualified = prefix.to_vec();
                if !qualified.is_empty() {
                    qualified.push(u16::from(b'.'));
                }
                qualified.extend_from_slice(&name);
                qualified
            });
            let nested_prefix = name.as_deref().unwrap_or(prefix);
            if rule.rule_type() == NativeRuleType::Import {
                if let Some(imported) = sheet.imported_sheet(rule.identity()) {
                    imported.visit_effective_rule_data(order, nested_prefix, visit);
                }
            } else if !matches!(rule.rule_type(), NativeRuleType::Function | NativeRuleType::Keyframes)
                && rule.cached_condition_holds()
            {
                if let Some(container) = rule.container() {
                    containers.push(container.clone());
                }
                let _ = rule.visit_children(&mut |child| {
                    walk(sheet, child, order, nested_prefix, containers, visit);
                    std::ops::ControlFlow::Continue(())
                });
                if rule.container().is_some() {
                    containers.pop();
                }
            }
            if order == FfiRuleTraversalOrder::Postorder {
                visit(rule, prefix, containers);
            }
        }
        let mut containers = Vec::new();
        let _ = self.rules.visit_rules(&mut |rule| {
            walk(self, rule, order, prefix, &mut containers, visit);
            std::ops::ControlFlow::Continue(())
        });
    }

    pub(crate) fn new(rules: Rc<NativeRuleList>, media: MediaList) -> Rc<Self> {
        Rc::new(Self {
            identity: NEXT_SHEET_IDENTITY
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(1))
                .expect("stylesheet identity overflow"),
            rules,
            media,
            flags: Cell::new(NativeStyleSheetFlag::OriginClean as u8),
            media_state: Cell::new(NativeStyleSheetMediaState::Unevaluated),
            imports: RefCell::default(),
        })
    }
}

/// Visit immutable rule inputs without creating document-local rule or payload owners.
///
/// # Safety
/// The callback must not mutate the sheet graph or retain the borrowed view or layer text.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_visit_effective_rule_data(
    sheet: &NativeStyleSheet,
    order: FfiRuleTraversalOrder,
    context: *const c_void,
    visit: unsafe extern "C" fn(*const c_void, &NativeRuleView<'_>, FfiUtf16View),
) {
    sheet.visit_effective_rule_data(order, &[], &mut |rule, prefix, containers| unsafe {
        visit(
            context,
            &NativeRuleView { rule, containers },
            FfiUtf16View {
                ascii: std::ptr::null(),
                utf16: prefix.as_ptr(),
                length: prefix.len(),
            },
        );
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_identity(sheet: &NativeStyleSheet) -> u64 {
    sheet.identity()
}

impl Drop for NativeStyleSheet {
    fn drop(&mut self) {
        // Import depth is independent of parser nesting limits. Release the graph iteratively
        // when the last owner goes away, leaving independently retained descendants alive.
        let mut pending: Vec<_> = self.imports.get_mut().drain().map(|(_, child)| child).collect();
        while let Some(child) = pending.pop() {
            if let Ok(mut child) = Rc::try_unwrap(child) {
                pending.extend(child.imports.get_mut().drain().map(|(_, child)| child));
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_create(
    rules: *const NativeRuleList,
    media: &MediaList,
) -> *const NativeStyleSheet {
    unsafe { Rc::increment_strong_count(rules) };
    Rc::into_raw(NativeStyleSheet::new(unsafe { Rc::from_raw(rules) }, media.clone()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_release(sheet: *const NativeStyleSheet) {
    if !sheet.is_null() {
        drop(unsafe { Rc::from_raw(sheet) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_rules(sheet: &NativeStyleSheet) -> *const NativeRuleList {
    Rc::as_ptr(&sheet.rules)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_media(sheet: &NativeStyleSheet) -> *const MediaList {
    &raw const sheet.media
}

/// Attach a loaded sheet to its import rule, or remove the edge when the rule is detached.
///
/// # Safety
/// A non-null imported sheet must be live and must not transitively import this sheet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_set_import(
    sheet: &NativeStyleSheet,
    rule_identity: u64,
    imported: *const NativeStyleSheet,
) {
    if imported.is_null() {
        sheet.imports.borrow_mut().remove(&rule_identity);
    } else {
        unsafe { Rc::increment_strong_count(imported) };
        sheet
            .imports
            .borrow_mut()
            .insert(rule_identity, unsafe { Rc::from_raw(imported) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_import(sheet: &NativeStyleSheet, rule_identity: u64) -> *const NativeStyleSheet {
    sheet
        .imports
        .borrow()
        .get(&rule_identity)
        .map_or(std::ptr::null(), |imported| Rc::into_raw(imported.clone()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_media_state(sheet: &NativeStyleSheet) -> NativeStyleSheetMediaState {
    sheet.media_state.get()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_reset_media_state(sheet: &NativeStyleSheet) {
    sheet.media_state.set(NativeStyleSheetMediaState::Unevaluated);
}

/// Evaluate media queries throughout the sheet graph from one document environment snapshot.
///
/// # Safety
/// Engine must be exclusively available and the environment must contain valid media feature data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_evaluate_media_queries(
    sheet: &NativeStyleSheet,
    environment: FfiMediaEnvironment,
    state: &mut NativeMediaEvaluationState,
    engine: *mut c_void,
) -> NativeStyleSheetMediaEvaluation {
    let engine: &mut crate::css::style::StyleEngine = unsafe { &mut *engine.cast() };
    sheet.evaluate_media_queries(unsafe { environment.borrow() }, state, &mut |identity, holds| {
        if let Some(rule) = engine.native_rule_id(identity) {
            crate::css::style::bridge::operations::set_rule_conditions_hold(engine, rule.0 + 1, holds);
        }
    })
}

/// Publish inherited condition gates directly into the document engine.
///
/// # Safety
/// Engine and environment must be live and exclusively available for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_publish_conditions(
    sheet: &NativeStyleSheet,
    engine: *mut c_void,
    environment: FfiMediaEnvironment,
) {
    sheet.publish_conditions(unsafe { &mut *engine.cast() }, unsafe { environment.borrow() });
}

// Keep first-seen sibling order and emit descendants before their parent, including the
// implicit outer layer. Names remain UTF-16 and each distinct qualified name is copied once.
fn cascade_layer_order<'a>(sheets: impl IntoIterator<Item = &'a NativeStyleSheet>) -> Vec<Vec<u16>> {
    let mut names = HashMap::<Vec<u16>, usize>::new();
    let mut children = vec![Vec::<usize>::new()];
    let mut prefix = Vec::new();
    let mut traversal_prefix = Vec::new();
    for sheet in sheets {
        sheet.visit_layer_names(&mut traversal_prefix, &mut |name| {
            prefix.clear();
            let mut parent = 0;
            for part in name
                .split(|&unit| unit == u16::from(b'.'))
                .filter(|part| !part.is_empty())
            {
                if !prefix.is_empty() {
                    prefix.push(u16::from(b'.'));
                }
                prefix.extend_from_slice(part);
                parent = if let Some(&index) = names.get(prefix.as_slice()) {
                    index
                } else {
                    let index = children.len();
                    names.insert(prefix.clone(), index);
                    children[parent].push(index);
                    children.push(Vec::new());
                    index
                };
            }
        });
    }
    let mut qualified = vec![Vec::new(); children.len()];
    for (name, index) in names {
        qualified[index] = name;
    }
    fn flatten(index: usize, children: &[Vec<usize>], names: &mut [Vec<u16>], result: &mut Vec<Vec<u16>>) {
        for &child in &children[index] {
            flatten(child, children, names, result);
        }
        result.push(std::mem::take(&mut names[index]));
    }
    let mut result = Vec::with_capacity(children.len());
    flatten(0, &children, &mut qualified, &mut result);
    result
}

/// Publish the layer order of the host's ordered, active author sheets.
///
/// # Safety
/// Sheets and engine must be live. prepare must flush host changes without destroying them.
/// The engine is not borrowed during prepare.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_sheet_publish_layer_order(
    sheets: *const *const NativeStyleSheet,
    count: usize,
    engine: *mut c_void,
    tree_scope: u32,
    previously_had_layers: bool,
    context: *mut c_void,
    prepare: unsafe extern "C" fn(*mut c_void),
) -> bool {
    let sheets = if count == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(sheets, count) }
    };
    let names = cascade_layer_order(sheets.iter().map(|&sheet| unsafe { &*sheet }));
    let has_layers = names.len() > 1;
    // Unlayered attachment needs no topology transaction, but removing the last named layer
    // must clear the engine's old ranks.
    if has_layers || previously_had_layers {
        unsafe { prepare(context) };
        let engine = unsafe { &mut *engine.cast::<crate::css::style::StyleEngine>() };
        let layers: Vec<_> = names
            .iter()
            .map(|name| {
                if name.is_empty() {
                    0
                } else {
                    crate::css::style::bridge::intern_native_text(engine, name).0
                }
            })
            .collect();
        crate::css::style::bridge::operations::set_layer_order(engine, tree_scope, &layers);
    }
    has_layers
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_flag(sheet: &NativeStyleSheet, flag: NativeStyleSheetFlag) -> bool {
    sheet.flags.get() & flag as u8 != 0
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_sheet_set_flag(sheet: &NativeStyleSheet, flag: NativeStyleSheetFlag, value: bool) {
    let mask = flag as u8;
    sheet.flags.set(if value {
        sheet.flags.get() | mask
    } else {
        sheet.flags.get() & !mask
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::media_list::rust_media_list_length;
    use crate::css::parser::syntax_parser::{ParsedStyleSheet, parse_shared_stylesheet};
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::{rust_rule_list_at, rust_rule_list_count, rust_rule_list_remove};

    fn parse(source: &str) -> std::sync::Arc<ParsedStyleSheet> {
        let units: Vec<_> = source.encode_utf16().collect();
        // All fields are booleans, integers, or nullable pointers; zero is an empty parser context.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        unsafe { parse_shared_stylesheet(crate::css::css_tokenizer::TokenizerInput::Utf16(&units), &context) }
    }

    fn sheet(parsed: &std::sync::Arc<ParsedStyleSheet>) -> Rc<NativeStyleSheet> {
        NativeStyleSheet::new(
            NativeRuleList::from_parsed(parsed.clone()),
            MediaList::new(Default::default()),
        )
    }

    fn set_media(sheet: &NativeStyleSheet, text: &str) {
        let units: Vec<_> = text.encode_utf16().collect();
        sheet
            .media
            .set_text(crate::css::css_tokenizer::TokenizerInput::Utf16(&units));
    }

    #[test]
    fn media_cache_invalidation_reads_shared_rules_without_owners() {
        let environment = FfiMediaEnvironment {
            values: std::ptr::null(),
            value_count: 0,
            length_resolution_context: std::ptr::null(),
        };
        for (text, reaches_cache) in [
            ("@media all { .target { color: red; } }", false),
            ("@media all { @keyframes motion { from { opacity: 0; } } }", true),
            ("@container (width > 1px) { .target { color: red; } }", true),
        ] {
            let sheet = sheet(&parse(text));
            let owners = crate::css::rule::RULE_OWNER_ALLOCATIONS.get();
            let mut state = NativeMediaEvaluationState::default();
            let initial = sheet.evaluate_media_queries(unsafe { environment.borrow() }, &mut state, &mut |_, _| {});
            assert!(!initial.any_changed);
            assert!(!initial.style_cache_inputs_changed);
            set_media(&sheet, "not all");
            let changed = sheet.evaluate_media_queries(unsafe { environment.borrow() }, &mut state, &mut |_, _| {});
            assert!(changed.sheet_changed);
            assert_eq!(changed.style_cache_inputs_changed, reaches_cache);
            assert_eq!(crate::css::rule::RULE_OWNER_ALLOCATIONS.get(), owners);
        }
    }

    #[test]
    fn cascade_layers_merge_shared_sheets_and_imports_in_first_seen_order() {
        let parsed = parse("@import 'child.css' layer(輸入); @layer 外.後, 別; @layer 外 { @layer 前 {} }");
        let first = sheet(&parsed);
        let shared = sheet(&parsed);
        let child = sheet(&parse("@layer 子.葉, 子.先;"));
        let import = crate::css::rule::rust_rule_list_identity_at(&first.rules, 0);
        first.imports.borrow_mut().insert(import, child);
        let other = sheet(&parse("@layer 別.中, 外.後, 最後;"));
        let owners = crate::css::rule::RULE_OWNER_ALLOCATIONS.get();
        let names = cascade_layer_order([first.as_ref(), shared.as_ref(), other.as_ref()]);
        assert_eq!(
            names,
            [
                "輸入.子.葉",
                "輸入.子.先",
                "輸入.子",
                "輸入",
                "外.後",
                "外.前",
                "外",
                "別.中",
                "別",
                "最後",
                ""
            ]
            .map(|name| name.encode_utf16().collect::<Vec<_>>())
        );
        assert_eq!(crate::css::rule::RULE_OWNER_ALLOCATIONS.get(), owners);
        assert_eq!(cascade_layer_order([]), [Vec::<u16>::new()]);
        set_media(&first, "not all");
        assert_eq!(cascade_layer_order([first.as_ref()]), [Vec::<u16>::new()]);
    }

    #[test]
    fn shared_parse_has_independent_live_rules_media_and_flags() {
        let parsed = parse(".文字 { width: 13px; } .雪 { width: 17px; }");
        let first = sheet(&parsed);
        set_media(&first, "screen");
        let second = NativeStyleSheet::new(NativeRuleList::from_parsed(parsed.clone()), first.media.share());
        assert!(rust_style_sheet_flag(&first, NativeStyleSheetFlag::OriginClean));
        for flag in [
            NativeStyleSheetFlag::Disabled,
            NativeStyleSheetFlag::Alternate,
            NativeStyleSheetFlag::Constructed,
            NativeStyleSheetFlag::DisallowModification,
        ] {
            let mask = flag as u8;
            assert_eq!(first.flags.get() & mask, 0);
            rust_style_sheet_set_flag(&first, flag, true);
            assert_ne!(first.flags.get() & mask, 0);
            assert_eq!(second.flags.get() & mask, 0);
        }
        rust_style_sheet_set_flag(&first, NativeStyleSheetFlag::OriginClean, false);
        rust_style_sheet_set_flag(&first, NativeStyleSheetFlag::Disabled, false);
        assert!(rust_style_sheet_flag(&first, NativeStyleSheetFlag::Constructed));
        assert!(rust_style_sheet_flag(&second, NativeStyleSheetFlag::OriginClean));
        rust_rule_list_remove(&first.rules, 0);
        set_media(&first, "print, screen");
        assert_eq!(rust_rule_list_count(&second.rules), 2);
        assert_eq!(unsafe { rust_media_list_length(&second.media) }, 1);
        assert_eq!(rust_rule_list_count(&sheet(&parsed).rules), 2);
        drop(parsed);

        let retained = first.clone();
        drop(first);
        let rules = retained.rules.clone();
        let media = retained.media.clone();
        drop(retained);
        assert_eq!(rust_rule_list_count(&rules), 1);
        assert_eq!(unsafe { rust_media_list_length(&media) }, 2);
    }

    fn layer_names(sheet: &NativeStyleSheet) -> Vec<Vec<u16>> {
        let mut names = Vec::new();
        let mut prefix: Vec<_> = "根".encode_utf16().collect();
        sheet.visit_layer_names(&mut prefix, &mut |name| names.push(name.to_vec()));
        assert_eq!(prefix, "根".encode_utf16().collect::<Vec<_>>());
        names
    }

    fn assert_layers(sheet: &NativeStyleSheet, expected: &[&str]) {
        assert_eq!(
            layer_names(sheet),
            expected
                .iter()
                .map(|name| name.encode_utf16().collect::<Vec<_>>())
                .collect::<Vec<_>>()
        );
    }

    #[test]
    fn effective_rules_preserve_order_conditions_and_layer_context() {
        use crate::css::container_conditions::rust_container_conditions_contains_size_feature;
        use crate::css::rule::NativeRuleType as Type;

        let sheet = sheet(&parse(
            "@import 'one.css' layer(入口); @layer 外 { @layer 内 { .子 {} } @media not all { .隠 {} } @supports (unknown: value) { .無 {} } @supports (display: block) { .有 {} } @container (width > 999px) { .幅 {} } @function --値() { @media all { result: 1; } } @keyframes 動 { from { opacity: 0; } } }",
        ));
        assert_eq!(rust_rule_list_count(&sheet.rules), 2);
        #[derive(Default)]
        struct Visited {
            types: Vec<u16>,
            layers: Vec<Vec<u16>>,
        }
        let visit = |order| {
            let owners = crate::css::rule::RULE_OWNER_ALLOCATIONS.get();
            let mut visited = Visited::default();
            let prefix: Vec<_> = "根".encode_utf16().collect();
            sheet.visit_effective_rule_data(order, &prefix, &mut |rule, prefix, _| {
                visited.types.push(rule.rule_type() as u16);
                visited.layers.push(prefix.to_vec());
                if rule.rule_type() == Type::Container {
                    assert!(rust_container_conditions_contains_size_feature(
                        rule.container().unwrap()
                    ));
                }
            });
            assert_eq!(crate::css::rule::RULE_OWNER_ALLOCATIONS.get(), owners);
            visited
        };
        let check = |visited: Visited, types: &[Type], layers: &[&str]| {
            assert_eq!(visited.types, types.iter().map(|kind| *kind as u16).collect::<Vec<_>>());
            assert_eq!(
                visited.layers,
                layers
                    .iter()
                    .map(|layer| layer.encode_utf16().collect::<Vec<_>>())
                    .collect::<Vec<_>>()
            );
        };
        check(
            visit(FfiRuleTraversalOrder::Preorder),
            &[
                Type::Import,
                Type::LayerBlock,
                Type::LayerBlock,
                Type::Style,
                Type::Media,
                Type::Supports,
                Type::Supports,
                Type::Style,
                Type::Container,
                Type::Style,
                Type::Function,
                Type::Keyframes,
            ],
            &[
                "根",
                "根",
                "根.外",
                "根.外.内",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
            ],
        );
        check(
            visit(FfiRuleTraversalOrder::Postorder),
            &[
                Type::Import,
                Type::Style,
                Type::LayerBlock,
                Type::Media,
                Type::Supports,
                Type::Style,
                Type::Supports,
                Type::Style,
                Type::Container,
                Type::Function,
                Type::Keyframes,
                Type::LayerBlock,
            ],
            &[
                "根",
                "根.外.内",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根.外",
                "根",
            ],
        );
    }

    #[test]
    fn effective_rules_follow_native_imports_in_both_orders() {
        let root = sheet(&parse("@import 'middle.css' layer(外); .最後 {}"));
        let middle = sheet(&parse("@import 'leaf.css' layer(中); .隣 {}"));
        let leaf = sheet(&parse(".雪 {}"));
        let identity = |sheet: &NativeStyleSheet, index: usize| {
            let mut identities = Vec::new();
            let _ = sheet.rules.visit_rules(&mut |rule| {
                identities.push(rule.identity());
                std::ops::ControlFlow::Continue(())
            });
            identities[index]
        };
        let root_import = identity(&root, 0);
        let middle_import = identity(&middle, 0);
        let leaf_rule = identity(&leaf, 0);
        let middle_rule = identity(&middle, 1);
        let root_rule = identity(&root, 1);
        unsafe {
            rust_style_sheet_set_import(&root, root_import, Rc::as_ptr(&middle));
            rust_style_sheet_set_import(&middle, middle_import, Rc::as_ptr(&leaf));
        }
        let owners = crate::css::rule::RULE_OWNER_ALLOCATIONS.get();
        let mut prefix = Vec::with_capacity(32);
        prefix.extend("根".encode_utf16());
        let storage = prefix.as_ptr();
        let mut layers = Vec::new();
        root.visit_layer_names(&mut prefix, &mut |name| {
            assert_eq!(name.as_ptr(), storage);
            layers.push(name.to_vec());
        });
        assert_eq!(
            layers,
            ["根.外.中", "根.外"].map(|name| name.encode_utf16().collect::<Vec<_>>())
        );
        assert_eq!(prefix, "根".encode_utf16().collect::<Vec<_>>());
        assert_eq!(crate::css::rule::RULE_OWNER_ALLOCATIONS.get(), owners);
        let collect = |order| {
            let owners = crate::css::rule::RULE_OWNER_ALLOCATIONS.get();
            let mut rules: Vec<(u64, Vec<u16>)> = Vec::new();
            let prefix: Vec<_> = "根".encode_utf16().collect();
            root.visit_effective_rule_data(order, &prefix, &mut |rule, prefix, _| {
                rules.push((rule.identity(), prefix.to_vec()));
            });
            assert_eq!(crate::css::rule::RULE_OWNER_ALLOCATIONS.get(), owners);
            rules
        };
        let entry = |identity, prefix: &str| (identity, prefix.encode_utf16().collect::<Vec<_>>());
        assert_eq!(
            collect(FfiRuleTraversalOrder::Preorder),
            vec![
                entry(root_import, "根"),
                entry(middle_import, "根.外"),
                entry(leaf_rule, "根.外.中"),
                entry(middle_rule, "根.外"),
                entry(root_rule, "根"),
            ]
        );
        assert_eq!(
            collect(FfiRuleTraversalOrder::Postorder),
            vec![
                entry(leaf_rule, "根.外.中"),
                entry(middle_import, "根.外"),
                entry(middle_rule, "根.外"),
                entry(root_import, "根"),
                entry(root_rule, "根"),
            ]
        );
        set_media(&middle, "not all");
        assert_eq!(
            collect(FfiRuleTraversalOrder::Preorder),
            vec![entry(root_import, "根"), entry(root_rule, "根")]
        );
        set_media(&middle, "");
        unsafe { rust_style_sheet_set_import(&middle, middle_import, std::ptr::null()) };
        assert_eq!(
            collect(FfiRuleTraversalOrder::Preorder),
            vec![
                entry(root_import, "根"),
                entry(middle_import, "根.外"),
                entry(middle_rule, "根.外"),
                entry(root_rule, "根"),
            ]
        );
        set_media(&root, "not all");
        assert!(collect(FfiRuleTraversalOrder::Preorder).is_empty());
    }

    #[test]
    fn imports_retain_children_until_replaced_or_detached() {
        let root = sheet(&parse("@import 'middle.css' layer(外); @layer 末;"));
        let middle = sheet(&parse("@import 'leaf.css' layer(中); @layer 隣;"));
        let leaf = sheet(&parse("@layer 内;"));
        let root_import = unsafe { &*rust_rule_list_at(&root.rules, 0) };
        let middle_import = unsafe { &*rust_rule_list_at(&middle.rules, 0) };
        let root_id = crate::css::rule::rust_rule_identity(root_import);
        let middle_id = crate::css::rule::rust_rule_identity(middle_import);
        unsafe {
            rust_style_sheet_set_import(&middle, middle_id, Rc::as_ptr(&leaf));
            rust_style_sheet_set_import(&root, root_id, Rc::as_ptr(&middle));
        }
        let weak_middle = Rc::downgrade(&middle);
        let weak_leaf = Rc::downgrade(&leaf);
        drop(middle);
        drop(leaf);
        assert_layers(&root, &["根.外.中.内", "根.外.中", "根.外.隣", "根.外", "根.末"]);
        let middle = weak_middle.upgrade().unwrap();
        let leaf = weak_leaf.upgrade().unwrap();
        let replacement = sheet(&parse("@layer 替;"));
        unsafe { rust_style_sheet_set_import(&root, root_id, Rc::as_ptr(&replacement)) };
        drop(replacement);
        assert_layers(&root, &["根.外.替", "根.外", "根.末"]);
        unsafe { rust_style_sheet_set_import(&root, root_id, std::ptr::null()) };
        assert!(rust_style_sheet_import(&root, root_id).is_null());
        assert_layers(&root, &["根.外", "根.末"]);
        assert_layers(&middle, &["根.中.内", "根.中", "根.隣"]);
        set_media(&leaf, "not all");
        assert_layers(&middle, &["根.中", "根.隣"]);
        drop(middle);
        assert!(weak_middle.upgrade().is_none());
        let rules = leaf.rules.clone();
        drop(leaf);
        assert!(weak_leaf.upgrade().is_none());
        assert_eq!(rust_rule_list_count(&rules), 1);
    }

    #[test]
    fn deep_import_chains_release_iteratively() {
        let parsed = parse("@import 'child.css';");
        let mut child = sheet(&parsed);
        let leaf = Rc::downgrade(&child);
        for _ in 0..20_000 {
            let parent = sheet(&parsed);
            let rule = unsafe { &*rust_rule_list_at(&parent.rules, 0) };
            parent
                .imports
                .borrow_mut()
                .insert(crate::css::rule::rust_rule_identity(rule), child);
            child = parent;
        }
        drop(parsed);
        drop(child);
        assert!(leaf.upgrade().is_none());
    }
}
