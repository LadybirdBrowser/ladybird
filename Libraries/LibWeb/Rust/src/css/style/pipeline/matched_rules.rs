/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rules the engine matches to an element, in the order its cascade applies them, which tests
//! read to see what an element's matching decided without reading its computed style.

use super::super::engine_calls::with_engine;
use super::super::*;
use super::bloom::AncestorFilter;
use super::matching::{ElementMatch, Matcher};
use super::rules::RuleIndex;
use crate::css::selector::Specificity;
use crate::render_state::{BegunRead, DocumentHost};

/// One rule matching an element, for the element at `node` among the elements asked about.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiMatchedRule {
    /// The position of the element among those asked about.
    pub node: u32,
    /// The rule identity as the boundary numbers it, one more than the engine's, or zero where
    /// the element could not be matched.
    pub rule: u32,
    /// The pseudo-element the rule targets, or `u32::MAX` for the element itself.
    pub pseudo_element: u32,
}

/// The rules that match one element, one per rule and target with the greatest specificity among
/// its matching selectors, in the order the cascade applies them. User agent rules are left out
/// unless `include_user_agent` says otherwise.
fn matched_rules(
    state: &RetainedState,
    index: &RuleIndex,
    matcher: &mut Matcher<'_>,
    node: StyleNodeID,
    include_user_agent: bool,
) -> Result<Vec<(RuleID, u8)>, Incomplete> {
    let projection = &state.projection;
    let Some(facts) = projection.facts(node) else {
        return Ok(Vec::new());
    };
    let mut filter = AncestorFilter::new();
    for ancestor_row in projection
        .ancestors(node)
        .filter_map(|ancestor| projection.row_of(ancestor))
    {
        for key in super::ancestor_keys_of(ancestor_row) {
            filter.insert(key);
        }
    }
    let mut matches: Vec<ElementMatch> = Vec::new();
    matcher.match_element(node, facts, &filter, &mut matches)?;

    // NB: Container conditions are not applied: the rules behind them are reported as matching,
    //     whatever the container they ask about says.
    let mut merged: Vec<(RuleID, u8, TreeScopeID, Specificity, u32, u32)> = Vec::new();
    for matched in &matches {
        if !include_user_agent && index.entries.placement(matched.entry as usize).origin == CascadeOrigin::UserAgent {
            continue;
        }
        let same_block = |existing: &&mut (RuleID, u8, TreeScopeID, Specificity, u32, u32)| {
            existing.0 == matched.rule && existing.1 == matched.pseudo_kind && existing.2 == matched.scope
        };
        match merged.iter_mut().find(same_block) {
            Some(existing) => {
                existing.3 = existing.3.max(matched.specificity);
                existing.4 = existing.4.min(matched.scope_proximity);
            }
            None => merged.push((
                matched.rule,
                matched.pseudo_kind,
                matched.scope,
                matched.specificity,
                matched.scope_proximity,
                matched.entry,
            )),
        }
    }
    let context_depth_of = |entry: u32| {
        matches
            .iter()
            .find(|matched| matched.entry == entry)
            .map_or(0, |matched| matched.context_depth)
    };
    let mut ordered: Vec<(CascadePriority, RuleID, u8)> = merged
        .into_iter()
        .map(|(rule, pseudo_kind, _, specificity, scope_proximity, first_entry)| {
            let entry = index.entries.view(first_entry as usize);
            let placement = index.entries.placement_of(entry);
            let priority = CascadePriority::new(PriorityInputs {
                origin: placement.origin,
                important: false,
                context_depth: context_depth_of(first_entry),
                element_attachment: ElementAttachment::Rule,
                layer_rank: placement.layer_rank,
                specificity,
                scope_proximity,
                sheet_rank: placement.sheet_rank,
                rule_rank: entry.rule_rank(),
            });
            (priority, rule, pseudo_kind)
        })
        .collect();
    ordered.sort_by_key(|(priority, _, _)| *priority);
    Ok(ordered
        .into_iter()
        .map(|(_, rule, pseudo_kind)| (rule, pseudo_kind))
        .collect())
}

impl StyleEngine {
    /// The rules matching each of `nodes`, with the index of the engine where it is live, or one
    /// built from the document's rules as a cold pass builds it where it is not.
    fn matched_rules(&self, nodes: &[u32], include_user_agent: bool) -> Vec<FfiMatchedRule> {
        let state = &self.retained;
        let cold;
        let index: &RuleIndex = match state.live_style.as_deref() {
            Some(live) => live.index(),
            None => {
                cold = RuleIndex::build(&state.rules, state.process_rule_indexes());
                &cold
            }
        };
        let mut matcher = Matcher::new(index, state);
        let mut out = Vec::new();
        for (position, &raw) in nodes.iter().enumerate() {
            let position = u32::try_from(position).expect("element count fits the boundary");
            let answer = StyleNodeID::from_raw(raw)
                .map(|node| matched_rules(state, index, &mut matcher, node, include_user_agent));
            let Some(Ok(rules)) = answer else {
                out.push(FfiMatchedRule {
                    node: position,
                    rule: 0,
                    pseudo_element: u32::MAX,
                });
                continue;
            };
            out.extend(rules.into_iter().map(|(rule, pseudo_kind)| FfiMatchedRule {
                node: position,
                rule: rule.0 + 1,
                pseudo_element: match pseudo_kind {
                    u8::MAX => u32::MAX,
                    kind => u32::from(kind),
                },
            }));
        }
        out
    }
}

/// Writes the rules the engine matches to each of the `node_count` elements at `nodes` into `out`,
/// element by element in the order asked and each element's in cascade order, with a rule of zero
/// for an element that could not be matched. User agent rules are written where
/// `include_user_agent` is set.
///
/// Returns the number written, or a count larger than `capacity` when nothing was written because
/// the buffer was too small.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `nodes` must point at
/// `node_count` readable identities, and `out` at `capacity` writable `FfiMatchedRule` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_matched_rules(
    host: &DocumentHost,
    read: &BegunRead,
    nodes: *const u32,
    node_count: usize,
    out: *mut FfiMatchedRule,
    capacity: usize,
    include_user_agent: bool,
) -> usize {
    // SAFETY: Guaranteed by the caller.
    let nodes = unsafe { super::super::bridge::borrow(nodes, node_count) };
    let rules = with_engine(read, host, |engine| engine.matched_rules(nodes, include_user_agent));
    if rules.len() > capacity {
        return rules.len();
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { std::ptr::copy_nonoverlapping(rules.as_ptr(), out, rules.len()) };
    rules.len()
}
