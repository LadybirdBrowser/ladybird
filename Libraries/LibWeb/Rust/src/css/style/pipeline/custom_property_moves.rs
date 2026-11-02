/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Moving the custom properties an element inherits without computing it again: where the
//! custom properties its parent hands down moved, an element substituting none of the ones that
//! moved keeps its style over its own custom properties built anew, and the move goes on to its
//! children only where those moved too.
//!
//! NB: This is independent inheritance: a child takes the independent inherited properties that
//!     changed without a restyle, here for the custom properties it substitutes none of.

use crate::css::custom_properties::CustomPropertyStore;
use std::sync::Arc;

/// The custom properties whose values differ between two stores, by the identities of their
/// names, remembered by the stores, which a thread's elements under one parent mostly share.
#[derive(Default)]
pub(super) struct MovedCustomProperties {
    by_stores: crate::fast_hash::FastMap<(usize, usize), Arc<[usize]>>,
    /// Keeps the stores whose addresses key `by_stores` alive, so no other store reuses one.
    stores: Vec<Arc<CustomPropertyStore>>,
}

impl MovedCustomProperties {
    /// The names whose values `new` holds otherwise than `old` does, sorted.
    pub(super) fn between(
        &mut self,
        old: Option<&Arc<CustomPropertyStore>>,
        new: Option<&Arc<CustomPropertyStore>>,
    ) -> Arc<[usize]> {
        let address = |store: Option<&Arc<CustomPropertyStore>>| store.map_or(0, |store| Arc::as_ptr(store) as usize);
        let key = (address(old), address(new));
        if key.0 == key.1 {
            return Arc::from([]);
        }
        if let Some(names) = self.by_stores.get(&key) {
            return names.clone();
        }
        // Every name either chain holds a value of is held by a store of it, at some depth.
        let below = self.between(
            old.and_then(|store| store.parent()),
            new.and_then(|store| store.parent()),
        );
        let mut names: Vec<usize> = old
            .into_iter()
            .chain(new)
            .flat_map(|store| store.own_values.keys())
            .chain(below.iter().copied())
            .collect();
        names.sort_unstable();
        names.dedup();
        names.retain(|&name| value_moved(name, old, new));
        let names: Arc<[usize]> = names.into();
        self.stores.extend(old.cloned());
        self.stores.extend(new.cloned());
        self.by_stores.insert(key, names.clone());
        names
    }
}

fn value_moved(name: usize, old: Option<&Arc<CustomPropertyStore>>, new: Option<&Arc<CustomPropertyStore>>) -> bool {
    match (
        old.and_then(|store| store.get(name)),
        new.and_then(|store| store.get(name)),
    ) {
        (None, None) => false,
        (Some(old), Some(new)) => !crate::css::style_value::retained_style_values_equal(&old.value, &new.value),
        _ => true,
    }
}

/// Whether an element substituting `reads` computes the same over `new`, its custom properties
/// now, as it did over the ones it held, where `moved` are the names whose values differ between
/// them: it substitutes none of those, nor one whose value waits on substitutions of its own,
/// which may read one that moved.
pub(super) fn reads_none_moved(
    reads: &[Arc<[u16]>],
    moved: &[usize],
    old: Option<&Arc<CustomPropertyStore>>,
    new: Option<&Arc<CustomPropertyStore>>,
) -> bool {
    if reads.is_empty() {
        return true;
    }
    let moved_names = moved
        .iter()
        .filter_map(|&name| {
            old.and_then(|store| store.get(name))
                .or_else(|| new.and_then(|store| store.get(name)))
        })
        .map(|entry| &entry.name);
    for name in moved_names {
        if reads
            .binary_search_by(|read| read.as_ref().cmp(&*name.to_utf16()))
            .is_ok()
        {
            return false;
        }
    }
    reads.iter().all(|read| {
        [old, new].into_iter().all(|store| {
            store.is_none_or(|store| {
                store
                    .resolved_value_identity(&ak::Utf16FlyString::from_utf16(read))
                    .is_some()
            })
        })
    })
}

#[cfg(test)]
mod tests {
    use super::super::super::TreeScopeID;
    use super::super::test_support::*;

    const RULES: &str = ".theme { --theme: 1px; } .other-theme { --theme: 2px; } \
                         .own { --own: 3px; width: var(--own); } .reader { height: var(--theme); } \
                         .stop { --theme: 5px; } .alias { --alias: var(--theme); } \
                         .alias-reader { width: var(--alias); }";

    /// Have `root -> a -> b -> c` give `b` and `c` the classes `classes`, then move the theme `a`
    /// hands down, check the engine against a cold pass, and answer which of `a`, `b` and `c`
    /// the pass after the move styled.
    fn styled_after_theme_move(classes: [&str; 2]) -> [bool; 3] {
        let (mut engine, nodes) = nested_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, RULES);
        add_class(&mut engine, nodes[1], "theme");
        add_class(&mut engine, nodes[2], classes[0]);
        add_class(&mut engine, nodes[3], classes[1]);
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);

        remove_class(&mut engine, nodes[1], "theme");
        add_class(&mut engine, nodes[1], "other-theme");
        run_style_update(&mut engine, nodes[0]);
        let styled = engine
            .retained
            .live_counters
            .last_pass_elements(&engine.retained.projection, nodes[0], false)
            .to_vec();
        assert_live_matches_cold(&mut engine, nodes[0]);
        std::array::from_fn(|index| styled.contains(&nodes[index + 1]))
    }

    #[test]
    fn elements_reading_no_moved_custom_property_take_it_without_being_styled() {
        // `b` substitutes only the custom property it declares, `c` the one that moved.
        assert_eq!(styled_after_theme_move(["own", "reader"]), [true, false, true]);
    }

    #[test]
    fn a_custom_property_declared_over_a_moved_one_moves_with_it() {
        // `b` declares `--alias` over `--theme`, and `c` substitutes `--alias`.
        assert_eq!(styled_after_theme_move(["alias", "alias-reader"]), [true, false, true]);
    }

    #[test]
    fn a_move_stops_where_an_element_declares_the_moved_custom_property() {
        // `b` declares `--theme` itself, so `c` inherits what it did.
        assert_eq!(styled_after_theme_move(["stop", "reader"]), [true, false, false]);
    }

    #[test]
    fn an_element_reading_a_moved_custom_property_is_styled() {
        assert_eq!(styled_after_theme_move(["reader", "own"]), [true, true, false]);
    }
}
