/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[repr(C)]
pub struct FfiNativeRuleTarget {
    pub identity: u64,
    pub declaration_version: u32,
    pub source_identity: u64,
    pub declarations: *const c_void,
    pub layer_name: *const u16,
    pub layer_name_length: usize,
    pub has_container_conditions: bool,
    pub origin: FfiCascadeOrigin,
}

/// Issue a new identity for changed declaration contents, independently of CSSOM wrappers.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_next_declaration_block_version(host: &DocumentHost) -> u32 {
    host.next_declaration_block_version()
}

/// Queues a native declaration edit for the rule that owns the declarations, where the host published that rule to the
/// sheet `sheet`, and answers whether they declare transitions. The host is told first, with no engine borrowed.
///
/// # Safety
/// The callback must not mutate the native rule graph, and must remain valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_native_rule_declarations_changed(
    host: &DocumentHost,
    sheet: u32,
    rule: &crate::css::rule::NativeRule,
    context: *mut c_void,
    notify: unsafe extern "C" fn(*mut c_void),
) -> bool {
    let Some(identity) = rule
        .declaration_owner_identity()
        .filter(|&identity| host.published_rules().contains(sheet, identity))
    else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { notify(context) };
    let declarations = rule.cascade_declarations();
    let transitions = declarations
        .as_ref()
        .is_some_and(|declarations| declares_transitions(declarations.properties()));
    host.write_rules(RuleWrite::RuleDeclarations { identity, declarations });
    transitions
}

/// The native identity of the first rule after an inserted native subtree that the host published to the sheet
/// `sheet`, or 0 for none, without creating CSSOM objects.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_native_rule_successor(
    host: &DocumentHost,
    sheet: u32,
    native: &crate::css::style_sheet::NativeStyleSheet,
    identity: u64,
) -> u64 {
    host.published_rules().successor(sheet, native, identity)
}

/// Retires a native subtree from the sheet `sheet`, with host callbacks only for document and cascade-cache
/// notifications, and queues the removal of the rules the host published there.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and the native sheet, rule, callbacks, and any
/// non-null detached import must be live. Native rules must belong to Rc allocations. No graph borrow spans a host
/// callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_remove_native_rule(
    host: &DocumentHost,
    native: *const c_void,
    rule: *const c_void,
    detached_import: *const c_void,
    sheet: u32,
    context: *mut c_void,
    begin: unsafe extern "C" fn(*mut c_void, bool, bool),
    notify: unsafe extern "C" fn(*mut c_void, bool),
) {
    use crate::css::rule::{NativeRule, NativeRuleType, mutation, read::RuleRef};
    use crate::css::style_sheet::NativeStyleSheet;
    // SAFETY: Guaranteed by the caller.
    let removed = unsafe {
        mutation::removed_rules(
            &*rule.cast::<NativeRule>(),
            &*native.cast::<NativeStyleSheet>(),
            detached_import.cast::<NativeStyleSheet>().as_ref(),
        )
    };
    let changes_environment =
        RuleRef::Materialized(unsafe { &*rule.cast::<NativeRule>() }).change_needs_style_environment_bump();
    let has_counter_style = removed
        .iter()
        .any(|rule| RuleRef::Materialized(rule).rule_type() == NativeRuleType::CounterStyle);
    unsafe { begin(context, changes_environment, has_counter_style) };
    let mut published = Vec::new();
    for rule in removed {
        unsafe { notify(context, mutation::declares_layer(&rule)) };
        let identity = RuleRef::Materialized(&rule).identity();
        if host.published_rules().remove(sheet, identity) {
            published.push(identity);
        }
    }
    if !published.is_empty() {
        host.write_rules(RuleWrite::RemoveRules(published.into()));
    }
}

/// Read a rule's native cascade data without a CSSOM facade.
///
/// On success the caller owns declarations and must release it with
/// rust_declaration_data_release. Layer text is borrowed until the next rule mutation; callers must
/// copy any text needed across such a mutation. Source identity never retains a native sheet.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_native_rule_target(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    rule: u32,
    result: &mut FfiNativeRuleTarget,
) -> bool {
    with_engine(read, host, |engine| native_rule_target(engine, rule, result))
}

/// [`style_engine_native_rule_target`] on `engine`.
pub fn native_rule_target(engine: &mut StyleEngine, rule: u32, result: &mut FfiNativeRuleTarget) -> bool {
    let Some(target) = rule
        .checked_sub(1)
        .and_then(|id| engine.rules.native_target(RuleID(id)))
    else {
        return false;
    };
    let Some(declarations) = engine.current_rule_block(RuleID(rule - 1)) else {
        return false;
    };
    let origin = engine.rules.sheet_origin(engine.rules.rule_sheet(RuleID(rule - 1)));
    *result = FfiNativeRuleTarget {
        identity: target.identity.get(),
        declaration_version: engine
            .current_rule_version(RuleID(rule - 1))
            .declaration_block
            .map_or(0, |version| version.raw()),
        source_identity: target.source_identity,
        declarations: crate::css::declaration_block::SharedDeclarationBlock::into_raw(declarations).cast(),
        layer_name: target.layer_name().as_ptr(),
        layer_name_length: target.layer_name().len(),
        has_container_conditions: !target.containers().is_empty(),
        origin: match origin {
            CascadeOrigin::Author => FfiCascadeOrigin::Author,
            CascadeOrigin::AuthorPresentationalHint => FfiCascadeOrigin::AuthorPresentationalHint,
            CascadeOrigin::User => FfiCascadeOrigin::User,
            CascadeOrigin::UserAgent => FfiCascadeOrigin::UserAgent,
        },
    };
    true
}
