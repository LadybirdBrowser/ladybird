/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Moving the custom-property environments below an element whose own environment moved.

use std::ffi::c_void;

use super::inputs::{HeldCustomPropertyEnvironment, HostVarReads, RetainedCustomPropertyData};
use super::*;
use crate::css::custom_properties::CustomPropertyStore;

/// An environment the host holds, as a move names it: the identity, the store behind it, null for
/// the empty environment.
#[derive(Clone, Copy)]
pub(crate) struct NamedEnvironment {
    pub(crate) identity: u64,
    pub(crate) store: *const c_void,
}

/// A move of one element's custom-property environment, as the host made it.
pub(crate) struct EnvironmentMove {
    /// What the element's style resolved to before the move and resolves to now. A descendant
    /// reading a name whose value differs between the two computes again.
    pub(crate) old_base: NamedEnvironment,
    pub(crate) new_base: NamedEnvironment,
    /// What the element handed its children before the move.
    pub(crate) old_inheritable: u64,
    /// What it hands them now. Every element the move hands the environment it inherits takes
    /// this one, whose host object is `new_inheritable_data`, null for none.
    pub(crate) new_inheritable: NamedEnvironment,
    pub(crate) new_inheritable_data: *const c_void,
    /// Whether `new_inheritable` declares custom properties of its own.
    pub(crate) new_inheritable_declares: bool,
}

/// What the host does for an element a move reached.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum EnvironmentMoveAction {
    /// The element took the moved environment, and so did its synthetic pseudo-elements that held
    /// the one it held, `replaced`. The host installs `style_record`, its record republished over
    /// the moved environment, and moves the pseudo-elements whose environments it keeps itself.
    Republish {
        node: StyleNodeID,
        style_record: u64,
        replaced: u64,
    },
    /// The element declares custom properties of its own over the environment it inherits, and
    /// reads nothing that moved: the host builds them again over the moved environment, then
    /// moves the environments below the element in turn.
    Rebuild(StyleNodeID),
    /// The element's style reads the moved environment: it computes again, which reaches its own
    /// descendants in turn.
    Recompute(StyleNodeID),
}

/// The custom properties whose values differ between two environments, worked out once per pair
/// of stores for a style transaction. The names come from what either store holds itself, then
/// from the stores above them; two chains that share an ancestor pair share that pair's answer.
/// Most moves under a root that redefines hundreds of names meet the same root pair.
#[derive(Default)]
pub(super) struct ChangedCustomPropertyNames {
    /// By the addresses of the two stores, the raw names whose values differ, sorted.
    by_stores: HashMap<(usize, usize), Arc<[usize]>>,
    /// The stores the addresses name, kept alive so that no other store takes an address.
    stores: Vec<Arc<CustomPropertyStore>>,
}

impl ChangedCustomPropertyNames {
    pub(super) fn clear(&mut self) {
        self.by_stores.clear();
        self.stores.clear();
    }

    fn between(
        &mut self,
        old: Option<&Arc<CustomPropertyStore>>,
        new: Option<&Arc<CustomPropertyStore>>,
    ) -> Arc<[usize]> {
        let key = (
            old.map_or(0, |store| Arc::as_ptr(store) as usize),
            new.map_or(0, |store| Arc::as_ptr(store) as usize),
        );
        if key.0 == key.1 {
            return Arc::from([]);
        }
        if let Some(names) = self.by_stores.get(&key) {
            return names.clone();
        }
        let inherited = self.between(
            old.and_then(|store| store.parent()),
            new.and_then(|store| store.parent()),
        );
        let mut seen = HashSet::default();
        let mut names: Vec<usize> = old
            .into_iter()
            .chain(new)
            .flat_map(|store| store.own_values.keys().copied())
            .chain(inherited.iter().copied())
            .filter(|&name| seen.insert(name) && custom_property_value_moved(name, old, new))
            .collect();
        names.sort_unstable();
        let names: Arc<[usize]> = names.into();
        self.stores.extend(old.cloned());
        self.stores.extend(new.cloned());
        self.by_stores.insert(key, names.clone());
        names
    }
}

/// Whether the value one custom property has differs between two environments: a value pending
/// substitution differs from every value but itself. The engine's value equality compares an
/// unresolved value by its spelling, which the host's `StyleValue::equals` may not, so a reader
/// of such a value can be computed again where the host would hand it the moved environment.
fn custom_property_value_moved(
    name: usize,
    old: Option<&Arc<CustomPropertyStore>>,
    new: Option<&Arc<CustomPropertyStore>>,
) -> bool {
    let old = old.and_then(|store| store.get(name));
    let new = new.and_then(|store| store.get(name));
    let (old, new) = match (old, new) {
        (None, None) => return false,
        (Some(old), Some(new)) => (old.value.pointer(), new.value.pointer()),
        _ => return true,
    };
    if std::ptr::eq(old, new) {
        return false;
    }
    // SAFETY: Both values are kept alive by the stores that hold them.
    matches!(unsafe { &*old }, StyleValueData::PendingSubstitution { .. })
        || !unsafe { crate::css::style_value::rust_style_value_equals(old, new) }
}

/// A store the host names by its raw `Arc` pointer, with a reference of its own.
///
/// # Safety
/// `store` must be null or a live raw `Arc` pointer to a `CustomPropertyStore`.
unsafe fn retained_store(store: *const c_void) -> Option<Arc<CustomPropertyStore>> {
    if store.is_null() {
        return None;
    }
    let store = store.cast::<CustomPropertyStore>();
    unsafe {
        Arc::increment_strong_count(store);
        Some(Arc::from_raw(store))
    }
}

fn sorted_atoms_intersect(first: &[StyleAtomID], second: &[StyleAtomID]) -> bool {
    let (mut first, mut second) = (first.iter().peekable(), second.iter().peekable());
    while let (Some(a), Some(b)) = (first.peek(), second.peek()) {
        match a.cmp(b) {
            std::cmp::Ordering::Equal => return true,
            std::cmp::Ordering::Less => _ = first.next(),
            std::cmp::Ordering::Greater => _ = second.next(),
        }
    }
    false
}

impl StyleEngineState {
    /// Move the custom-property environments below `origin`, whose own moved as `moved` says, and
    /// answer what the host does for the elements the move reaches, in preorder. An element that
    /// holds the environment it inherits, and reads nothing that moved, takes the moved one here,
    /// with its record and its synthetic pseudo-elements, and the move goes on below it. It stops
    /// at an element that declares custom properties of its own, which the host rebuilds, and at
    /// one whose style reads the environment, which computes again.
    ///
    /// # Safety
    /// The stores `moved` names must be null or live raw `Arc` pointers to `CustomPropertyStore`s,
    /// and `moved.new_inheritable_data` must be null or a live `Web::CSS::CustomPropertyData`.
    pub(crate) unsafe fn move_custom_property_environment(
        &mut self,
        origin: StyleNodeID,
        moved: &EnvironmentMove,
        mut act: impl FnMut(EnvironmentMoveAction),
    ) {
        // Every record the move republishes names the environment it hands down.
        unsafe {
            self.retained
                .custom_property_environments
                .retain(moved.new_inheritable.identity, moved.new_inheritable.store);
        }
        let mut changed_names = None;
        let mut pending = Vec::new();
        self.push_flat_tree_element_children(origin, moved.old_inheritable, &mut pending);
        while let Some((element, old_parent_inheritable)) = pending.pop() {
            if let Some(old_inheritable) = unsafe {
                self.move_element_environment(element, old_parent_inheritable, moved, &mut changed_names, &mut act)
            } {
                self.push_flat_tree_element_children(element, old_inheritable, &mut pending);
            }
        }
    }

    /// Push the elements that inherit from `parent`, with what `parent` handed them before the
    /// move, so that they pop in flat tree order: its light children no slot takes, its shadow
    /// root's children, and the elements assigned to it as a slot.
    fn push_flat_tree_element_children(
        &self,
        parent: StyleNodeID,
        old_inheritable: u64,
        pending: &mut Vec<(StyleNodeID, u64)>,
    ) {
        let tree = &self.retained.tree;
        let start = pending.len();
        let mut next = tree.first_element_child(parent);
        while let Some(child) = next {
            next = tree.next_element_sibling(child);
            if tree.assigned_slot_of(child).is_none() {
                pending.push((child, old_inheritable));
            }
        }
        let mut next = tree
            .shadow_root_of(parent)
            .and_then(|root| tree.first_element_child(root));
        while let Some(child) = next {
            next = tree.next_element_sibling(child);
            pending.push((child, old_inheritable));
        }
        pending.extend(
            tree.assigned_nodes_of(parent)
                .iter()
                .filter(|node| node.element_index().is_some())
                .map(|&node| (node, old_inheritable)),
        );
        pending[start..].reverse();
    }

    /// Move one element, which held the environment `old_parent_inheritable` its parent handed it
    /// before the move. Answers what it handed its own children, when the move goes on below it.
    unsafe fn move_element_environment(
        &mut self,
        element: StyleNodeID,
        old_parent_inheritable: u64,
        moved: &EnvironmentMove,
        changed_names: &mut Option<Rc<[StyleAtomID]>>,
        act: &mut impl FnMut(EnvironmentMoveAction),
    ) -> Option<u64> {
        // An unstyled subtree takes whatever it inherits once it has style.
        let held_style_record = *self.retained.held_style_records.get(&element)?;
        let (existing, is_animation_overlay, declares) = self
            .retained
            .element_custom_property_data
            .get(&element)
            .map_or((0, false, false), |held| {
                (held.identity, held.sampled_over.is_some(), held.declares)
            });
        // An animation overlay samples over what the element's style resolves to, which a
        // computation moves.
        if is_animation_overlay {
            act(EnvironmentMoveAction::Recompute(element));
            return None;
        }
        // An element declaring no custom property of its own holds the environment it inherits,
        // whichever environment it holds it in; its cascade declaring some now is a computation's to
        // find.
        let holds_inherited_environment = existing == old_parent_inheritable
            || (!declares && !self.retained.node_declares_custom_properties(element));
        if !holds_inherited_environment {
            // Its declared values stand while it reads nothing that changed.
            if existing == 0 || !declares || self.environment_move_needs_recompute(element, moved, changed_names) {
                act(EnvironmentMoveAction::Recompute(element));
            } else {
                act(EnvironmentMoveAction::Rebuild(element));
            }
            return None;
        }
        if self.environment_move_needs_recompute(element, moved, changed_names) {
            act(EnvironmentMoveAction::Recompute(element));
            return None;
        }
        let new_inheritable = moved.new_inheritable.identity;
        if existing == new_inheritable {
            return None;
        }
        // What the element hands its children is what it holds, unless a registration keeps some
        // of its names from inheriting. Only a computation filters those out.
        if existing != old_parent_inheritable
            && self
                .retained
                .document_style_computation_inputs
                .custom_property_registry()
                .has_non_inheriting_registrations()
        {
            act(EnvironmentMoveAction::Recompute(element));
            return None;
        }
        let held = |declares| {
            (!moved.new_inheritable_data.is_null()).then(|| HeldCustomPropertyEnvironment {
                identity: new_inheritable,
                sampled_over: None,
                declares,
                // SAFETY: The host's object is live for the move, which takes a reference of its own.
                data: unsafe { RetainedCustomPropertyData::retain(moved.new_inheritable_data) },
            })
        };
        // A synthetic pseudo-element that held what the element held takes the moved environment;
        // one that resolved its own computes the element again.
        let mut pseudo_element_resolved_its_own = false;
        if let Some(environments) = self.retained.pseudo_element_custom_property_data.get_mut(&element) {
            environments.retain_mut(|(_, environment)| {
                if environment.identity != existing {
                    pseudo_element_resolved_its_own = true;
                    return true;
                }
                match held(false) {
                    Some(moved) => {
                        *environment = moved;
                        true
                    }
                    None => false,
                }
            });
            if environments.is_empty() {
                self.retained.pseudo_element_custom_property_data.remove(&element);
            }
        }
        if pseudo_element_resolved_its_own {
            act(EnvironmentMoveAction::Recompute(element));
        }
        match held(moved.new_inheritable_declares) {
            Some(moved) => _ = self.retained.element_custom_property_data.insert(element, moved),
            None => _ = self.retained.element_custom_property_data.remove(&element),
        }
        let republished = self.retained.republish_record_environment(element, new_inheritable);
        let style_record = republished.unwrap_or(held_style_record);
        act(EnvironmentMoveAction::Republish {
            node: element,
            style_record,
            replaced: existing,
        });
        Some(existing)
    }

    /// Whether a moved environment computes the element again rather than handing it the moved
    /// one: its style reads the environment other than through `var()`, or reads a name whose
    /// value moved through `var()`.
    fn environment_move_needs_recompute(
        &mut self,
        element: StyleNodeID,
        moved: &EnvironmentMove,
        changed_names: &mut Option<Rc<[StyleAtomID]>>,
    ) -> bool {
        if self.retained.element_recomputes_on_environment_move(element) {
            return true;
        }
        match self.retained.host_var_reads.get(&element) {
            Some(HostVarReads::Unknown) => true,
            // The names are worked out only once an element asks about them. A move that reaches
            // no element reading a name, such as an element arriving with its subtree, needs none.
            Some(HostVarReads::Names(reads)) if reads.is_empty() => false,
            Some(HostVarReads::Names(_)) => {
                let changed = changed_names
                    .get_or_insert_with(|| self.changed_custom_property_names(moved))
                    .clone();
                let Some(HostVarReads::Names(reads)) = self.retained.host_var_reads.get(&element) else {
                    unreachable!("the reads were just read");
                };
                sorted_atoms_intersect(reads, &changed)
            }
            // A record the engine computed: the engine knows whether it reads custom properties at
            // all.
            None => self.retained.node_style_reads_custom_properties(element),
        }
    }

    /// The custom properties whose values differ between what the moved element's style resolved
    /// to before and resolves to now, as the atoms the host publishes names it reads by. A name no
    /// host ever interned is read by no element.
    fn changed_custom_property_names(&mut self, moved: &EnvironmentMove) -> Rc<[StyleAtomID]> {
        // SAFETY: The move's stores are live for the move.
        let (old, new) = unsafe {
            (
                retained_store(moved.old_base.store),
                retained_store(moved.new_base.store),
            )
        };
        let names = self
            .host
            .environment_move_changed_names
            .between(old.as_ref(), new.as_ref());
        let atoms = &self.retained.atoms;
        let mut changed: Vec<StyleAtomID> = names.iter().filter_map(|&name| atoms.interned_raw(name)).collect();
        changed.sort_unstable();
        changed.into()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::tests::{discard_transaction, linear_document};

    fn data(address: usize) -> crate::css::style::inputs::RetainedCustomPropertyData {
        // SAFETY: A test's custom-property data is never reached through its address.
        unsafe { crate::css::style::inputs::RetainedCustomPropertyData::retain(address as *const c_void) }
    }

    fn moved(old_inheritable: u64, new_inheritable: u64) -> EnvironmentMove {
        let none = NamedEnvironment {
            identity: 0,
            store: std::ptr::null(),
        };
        EnvironmentMove {
            old_base: none,
            new_base: none,
            old_inheritable,
            new_inheritable: NamedEnvironment {
                identity: new_inheritable,
                store: std::ptr::null(),
            },
            new_inheritable_data: 0x2000 as *const c_void,
            new_inheritable_declares: true,
        }
    }

    /// A record for `node` to hold, published as one of no style groups.
    fn held_record(engine: &mut StyleEngine, node: StyleNodeID) -> u64 {
        let record = assigned_record(engine, node, 0);
        engine.set_held_style_record(node, record);
        record
    }

    /// A record of no style groups assigned to `node`, told apart by its dependency flags.
    fn assigned_record(engine: &mut StyleEngine, node: StyleNodeID, dependency_flags: u8) -> u64 {
        engine
            .publish_computed_groups(
                computed::ComputedStyleTarget::new(node, u8::MAX),
                &[],
                0,
                0,
                computed::ComputedMetadataInput {
                    pseudo_element_styles: 0,
                    dependency_flags,
                    counter_style_environment_identity: 0,
                    animation_overlay_identity: 0,
                    animated_overlay: crate::css::host_shared::HostShared::null(),
                    animation_overlay_payloads: &[],
                    longhand_table: crate::css::host_shared::HostShared::null(),
                },
            )
            .style_record_identity
            .raw()
    }

    /// The republished records a walk answered, by node and the environment they replaced, after
    /// checking that each names the moved environment.
    fn republished(engine: &StyleEngine, actions: &[EnvironmentMoveAction], moved: u64) -> Vec<(StyleNodeID, u64)> {
        actions
            .iter()
            .map(|action| {
                let EnvironmentMoveAction::Republish {
                    node,
                    style_record,
                    replaced,
                } = *action
                else {
                    panic!("{action:?} is no republish");
                };
                assert_eq!(
                    engine
                        .computed_group_sets
                        .style_record_custom_property_environment(style_record),
                    Some(moved)
                );
                (node, replaced)
            })
            .collect()
    }

    fn walk(engine: &mut StyleEngine, origin: StyleNodeID, moved: &EnvironmentMove) -> Vec<EnvironmentMoveAction> {
        let mut actions = Vec::new();
        unsafe { engine.move_custom_property_environment(origin, moved, |action| actions.push(action)) };
        actions
    }

    #[test]
    fn a_move_hands_its_environment_to_the_children_holding_the_old_one() {
        let (mut engine, nodes) = linear_document();
        discard_transaction(&mut engine);
        for &node in &nodes[1..] {
            held_record(&mut engine, node);
            engine.set_element_var_reads(node, true, true, &[]);
            engine.set_element_custom_property_data(node, Some(data(0x1000)), 1, None, true);
        }
        let actions = walk(&mut engine, nodes[0], &moved(1, 2));
        assert_eq!(
            republished(&engine, &actions, 2),
            nodes[1..].iter().map(|&node| (node, 1)).collect::<Vec<_>>()
        );
        for &node in &nodes[1..] {
            assert_eq!(engine.retained.element_custom_property_data[&node].identity, 2);
        }
    }

    #[test]
    fn a_move_stops_at_declarations_readers_overlays_and_unstyled_elements() {
        let (mut engine, nodes) = linear_document();
        discard_transaction(&mut engine);
        // An element declaring its own over another environment is rebuilt over the moved one.
        held_record(&mut engine, nodes[1]);
        engine.set_element_var_reads(nodes[1], true, true, &[]);
        engine.set_element_custom_property_data(nodes[1], Some(data(0x1000)), 3, None, true);
        // An element whose style reads the environment through if() computes again.
        held_record(&mut engine, nodes[2]);
        engine.set_element_recomputes_on_environment_move(nodes[2], true);
        engine.set_element_custom_property_data(nodes[2], Some(data(0x1000)), 1, None, false);
        // An unstyled element is left to the computation that gives it style.
        engine.set_element_custom_property_data(nodes[3], Some(data(0x1000)), 1, None, false);
        let actions = walk(&mut engine, nodes[0], &moved(1, 2));
        assert_eq!(
            actions,
            [
                EnvironmentMoveAction::Rebuild(nodes[1]),
                EnvironmentMoveAction::Recompute(nodes[2]),
            ]
        );

        engine.set_element_recomputes_on_environment_move(nodes[2], false);
        engine.set_element_custom_property_data(nodes[2], Some(data(0x1001)), 4, Some(1), false);
        // A C++ computation that reads more than its names say computes again too.
        engine.set_element_var_reads(nodes[1], true, false, &[]);
        let actions = walk(&mut engine, nodes[0], &moved(1, 2));
        assert_eq!(
            actions,
            [
                EnvironmentMoveAction::Recompute(nodes[1]),
                EnvironmentMoveAction::Recompute(nodes[2]),
            ]
        );
    }

    #[test]
    fn a_move_republishes_the_record_the_host_holds_beneath_a_newer_assignment() {
        let (mut engine, nodes) = linear_document();
        discard_transaction(&mut engine);
        held_record(&mut engine, nodes[1]);
        engine.set_element_var_reads(nodes[1], true, true, &[]);
        engine.set_element_custom_property_data(nodes[1], Some(data(0x1000)), 1, None, false);
        // A row the host has yet to apply installs the newer assignment, which moves more than the environment.
        let assigned = assigned_record(&mut engine, nodes[1], 1);
        let actions = walk(&mut engine, nodes[0], &moved(1, 2));
        assert_eq!(republished(&engine, &actions, 2), [(nodes[1], 1)]);
        let records = &engine.computed_group_sets;
        assert!(matches!(
            actions[..],
            [EnvironmentMoveAction::Republish { style_record, .. }] if records.style_record_dependency_flags(style_record) == Some(0)
        ));
        assert_eq!(
            records.assigned_style_record(nodes[1]).map(|record| record.raw()),
            Some(assigned)
        );
    }

    #[test]
    fn a_move_reads_no_changed_names_for_an_element_reading_none() {
        let (mut engine, nodes) = linear_document();
        discard_transaction(&mut engine);
        held_record(&mut engine, nodes[1]);
        engine.set_element_var_reads(nodes[1], true, true, &[]);
        engine.set_element_custom_property_data(nodes[1], Some(data(0x1000)), 1, None, false);
        let actions = walk(&mut engine, nodes[0], &moved(1, 2));
        assert_eq!(republished(&engine, &actions, 2), [(nodes[1], 1)]);
        assert!(engine.host.environment_move_changed_names.by_stores.is_empty());
    }
}
