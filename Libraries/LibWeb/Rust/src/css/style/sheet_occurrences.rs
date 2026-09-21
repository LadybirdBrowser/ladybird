/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::capacity::ShallowCapacityBytes;
use super::*;

struct SheetOccurrence {
    identity: u64,
    sheet: SheetID,
    conditions_hold: bool,
}

#[derive(Default)]
pub(super) struct ScopeSheetOccurrences {
    occurrences: Vec<SheetOccurrence>,
    published: Vec<SheetID>,
    accounted_bytes: u64,
}

impl StyleEngineState {
    pub(super) fn attach_sheet_occurrence(
        &mut self,
        sheet: SheetID,
        scope: TreeScopeID,
        identity: u64,
        before: u64,
        conditions_hold: bool,
        counters: &mut Counters,
    ) {
        let entries = &mut self.host.sheet_occurrences.entry(scope).or_default().occurrences;
        entries.retain(|entry| entry.identity != identity);
        let position = entries
            .iter()
            .position(|entry| entry.identity == before)
            .unwrap_or(entries.len());
        entries.insert(
            position,
            SheetOccurrence {
                identity,
                sheet,
                conditions_hold,
            },
        );
        self.publish_sheet_occurrences(scope, counters);
    }

    pub(super) fn detach_sheet_occurrence(&mut self, scope: TreeScopeID, identity: u64, counters: &mut Counters) {
        let Some(state) = self.host.sheet_occurrences.get_mut(&scope) else {
            return;
        };
        state.occurrences.retain(|entry| entry.identity != identity);
        self.publish_sheet_occurrences(scope, counters);
    }

    pub(super) fn set_sheet_occurrence_conditions(
        &mut self,
        scope: TreeScopeID,
        identity: u64,
        conditions_hold: bool,
        counters: &mut Counters,
    ) {
        let Some(entry) = self
            .host
            .sheet_occurrences
            .get_mut(&scope)
            .and_then(|state| state.occurrences.iter_mut().find(|entry| entry.identity == identity))
        else {
            return;
        };
        if entry.conditions_hold == conditions_hold {
            return;
        }
        entry.conditions_hold = conditions_hold;
        self.publish_sheet_occurrences(scope, counters);
    }

    fn publish_sheet_occurrences(&mut self, scope: TreeScopeID, counters: &mut Counters) {
        let state = self.host.sheet_occurrences.get_mut(&scope).unwrap();
        let mut seen = HashSet::default();
        let mut active: Vec<_> = state
            .occurrences
            .iter()
            .rev()
            .filter(|entry| entry.conditions_hold && seen.insert(entry.sheet))
            .map(|entry| entry.sheet)
            .collect();
        active.reverse();
        let old = std::mem::replace(&mut state.published, active);
        let active = state.published.clone();
        let bytes = state.occurrences.shallow_capacity_bytes() + state.published.shallow_capacity_bytes();
        self.host.sheet_occurrence_storage_bytes =
            self.host.sheet_occurrence_storage_bytes - state.accounted_bytes + bytes;
        state.accounted_bytes = bytes;
        if state.occurrences.is_empty() {
            self.host.sheet_occurrence_storage_bytes -= bytes;
            self.host.sheet_occurrences.remove(&scope);
            if self.host.sheet_occurrences.is_empty() {
                self.host.sheet_occurrences.shrink_to_fit();
            }
        }
        let bytes = self.host.sheet_occurrence_storage_bytes + self.host.sheet_occurrences.shallow_capacity_bytes();
        self.host
            .sheet_occurrence_memory
            .resize_required_to(&mut self.retained.memory, bytes);
        if old == active {
            return;
        }

        let old_set: HashSet<_> = old.iter().copied().collect();
        let active_set: HashSet<_> = active.iter().copied().collect();
        let order_changed = old
            .iter()
            .filter(|sheet| active_set.contains(sheet))
            .ne(active.iter().filter(|sheet| old_set.contains(sheet)));
        // Preserve legacy attachments, including the document's user and user-agent sheets.
        let mut sheets: Vec<_> = self
            .current_sheets_in_scope(scope)
            .iter()
            .copied()
            .filter(|sheet| !old_set.contains(sheet))
            .collect();
        sheets.extend_from_slice(&active);
        for &sheet in active.iter().filter(|sheet| !old_set.contains(sheet)) {
            self.restore_routing_for_reattached_sheet(sheet);
        }
        self.stage_sheets_in_scope(scope, sheets);
        for &sheet in old.iter().filter(|sheet| !active_set.contains(sheet)) {
            self.record_attachment(sheet, scope, true, false, counters);
            self.retained.routing_needs_detachment_sweep = true;
        }
        for &sheet in active.iter().filter(|sheet| !old_set.contains(sheet)) {
            self.record_attachment(sheet, scope, false, true, counters);
        }
        if order_changed {
            self.record_sheet_order_change(scope, counters);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn duplicate_occurrences_preserve_positions_while_inactive() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let shared = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let other = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
        let scope = TreeScopeID::DOCUMENT;
        engine.attach_sheet_occurrence(shared, scope, 1, 0, true);
        engine.attach_sheet_occurrence(other, scope, 2, 0, true);
        engine.attach_sheet_occurrence(shared, scope, 3, 0, true);
        assert_eq!(engine.current_sheets_in_scope(scope), &[other, shared]);

        engine.set_sheet_occurrence_conditions(scope, 3, false);
        assert_eq!(engine.current_sheets_in_scope(scope), &[shared, other]);
        engine.set_sheet_occurrence_conditions(scope, 1, false);
        assert_eq!(engine.current_sheets_in_scope(scope), &[other]);
        engine.set_sheet_occurrence_conditions(scope, 3, true);
        assert_eq!(engine.current_sheets_in_scope(scope), &[other, shared]);
        engine.set_sheet_occurrence_conditions(scope, 1, true);
        assert_eq!(engine.current_sheets_in_scope(scope), &[other, shared]);
        engine.detach_sheet_occurrence(scope, 3);
        assert_eq!(engine.current_sheets_in_scope(scope), &[shared, other]);
        engine.attach_sheet_occurrence(shared, scope, 1, 0, true);
        assert_eq!(engine.current_sheets_in_scope(scope), &[other, shared]);
        engine.attach_sheet_occurrence(shared, scope, 1, 2, true);
        assert_eq!(engine.current_sheets_in_scope(scope), &[shared, other]);
    }

    #[test]
    fn occurrence_conditions_are_local_to_their_scope_and_storage_is_released() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let shared = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let first = TreeScopeID(1);
        let second = TreeScopeID(2);
        engine.attach_sheet_occurrence(shared, first, 1, 0, true);
        engine.attach_sheet_occurrence(shared, second, 2, 0, true);
        engine.set_sheet_occurrence_conditions(first, 1, false);
        assert!(engine.current_sheets_in_scope(first).is_empty());
        assert_eq!(engine.current_sheets_in_scope(second), &[shared]);
        assert!(engine.program.sheet_conditions_hold(shared));
        engine.detach_sheet_occurrence(first, 1);
        engine.detach_sheet_occurrence(second, 2);
        assert!(engine.host.sheet_occurrences.is_empty());
        assert_eq!(engine.host.sheet_occurrences.capacity(), 0);
        assert_eq!(engine.host.sheet_occurrence_storage_bytes, 0);
    }
}
