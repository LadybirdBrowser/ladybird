/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS counters sets the layout tree build resolves, one per element and pseudo-element that
//! keeps a box, and the reads that generated content and list markers make of them.

use super::layout_node_arena::LayoutNodeArena;
use super::node_data::{
    GENERATED_FOR_AFTER, GENERATED_FOR_BACKDROP, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, pseudo_kind_of,
};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_string::CssString;
use crate::css::style::RetainedState;
use crate::css::style::tree::StyleNodeID;
use std::sync::Arc;

// "UAs may have implementation-specific limits on the maximum or minimum value of a counter.
// If a counter reset, set, or increment would push the value outside of that range, the value
// must be clamped to that range." - https://drafts.csswg.org/css-lists-3/#auto-numbering
pub(crate) type CounterValue = i32;

pub(crate) const LIST_ITEM_COUNTER_NAME: [u16; 9] = [
    b'l' as u16,
    b'i' as u16,
    b's' as u16,
    b't' as u16,
    b'-' as u16,
    b'i' as u16,
    b't' as u16,
    b'e' as u16,
    b'm' as u16,
];

pub(crate) fn is_list_item_counter_name(name: &[u16]) -> bool {
    name == LIST_ITEM_COUNTER_NAME
}

/// A counter name as a reader of the counters set has it.
pub(crate) enum CounterName<'a> {
    Units(&'a [u16]),
}

impl CounterName<'_> {
    fn matches(&self, counter_name: &CssString) -> bool {
        let counter_name = counter_name.units();
        match self {
            Self::Units(units) => *units == counter_name,
        }
    }

    fn to_css_string(&self) -> CssString {
        match self {
            Self::Units(units) => CssString::from_utf16(units),
        }
    }
}

/// An element or one of its pseudo-elements: what a counters set belongs to, and what creates a
/// counter.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct CounterOwner {
    pub(crate) element: StyleNodeID,
    /// The pseudo-element's `generated_for` code, or 0 for the element itself.
    pub(crate) generated_for: u8,
}

impl CounterOwner {
    pub(crate) fn element(element: StyleNodeID) -> Self {
        Self {
            element,
            generated_for: 0,
        }
    }

    /// The element and each of its pseudo-elements that can own a counters set or generated content.
    pub(crate) fn every_owner_of(element: StyleNodeID) -> impl Iterator<Item = Self> {
        [
            0,
            GENERATED_FOR_AFTER,
            GENERATED_FOR_BACKDROP,
            GENERATED_FOR_BEFORE,
            GENERATED_FOR_MARKER,
        ]
        .into_iter()
        .map(move |generated_for| Self { element, generated_for })
    }
}

// https://drafts.csswg.org/css-lists-3/#counter
#[derive(Clone, PartialEq)]
pub(crate) struct Counter {
    name: CssString,
    originating_element: CounterOwner, // "creator"
    reversed: bool,
    value: Option<CounterValue>,
}

impl Counter {
    pub(crate) fn new(name: CssString, originating_element: CounterOwner, reversed: bool, value: CounterValue) -> Self {
        Self {
            name,
            originating_element,
            reversed,
            value: Some(value),
        }
    }

    pub(crate) fn name(&self) -> &CssString {
        &self.name
    }

    pub(crate) fn originating_element(&self) -> CounterOwner {
        self.originating_element
    }

    pub(crate) fn reversed(&self) -> bool {
        self.reversed
    }

    pub(crate) fn value(&self) -> CounterValue {
        self.value.unwrap_or(0)
    }
}

// https://drafts.csswg.org/css-lists-3/#css-counters-set
// NB: Most elements inherit their parent's set unchanged, so a set is shared until it is written.
pub(crate) type CountersSet = Arc<Vec<Counter>>;

/// The counters set of every owner whose set is not the set of its parent element in the flat tree, keyed by its
/// owner. An owner whose set is absent has its parent's set; the set of a root element that is absent is empty.
// NB: Most elements have the set of their parent, so storing only the sets that differ keeps the map small, and
//     resolving the counters of an element that changes none writes nothing.
#[derive(Clone, Default)]
pub(crate) struct CountersSets {
    sets: crate::cow_map::CowMap<CounterOwner, CountersSet>,
    /// A bit per element, by its slot, that is clear where neither the element nor any of its pseudo-elements holds a
    /// set in `sets`.
    // OPTIMIZATION: A build asks of each owner it places whether the owner holds a set, which the bit answers without a
    //               lookup in the map for the owners that never held one.
    may_hold_sets: Arc<Vec<u64>>,
    list_item_counter_name: Option<CssString>,
    /// While a build resolves counters in tree order (see [`Self::begin_tree_order_walk`]), the owners it resolved that
    /// are open at the current point of the walk, each the parent of the next, with their sets: the ancestors of the
    /// next owner, and below the nearest, its preceding sibling and that sibling's last descendants.
    tree_order_walk: Option<Vec<(CounterOwner, Option<CountersSet>)>>,
}

impl CountersSets {
    /// Whether no element or pseudo-element has a counter.
    pub(crate) fn is_empty(&self) -> bool {
        self.sets.is_empty()
    }

    /// Starts a walk that resolves the counters of owners in tree order, each owner of the walk's span that takes part
    /// in counter resolution once, which lets each owner find what it inherits from the owners before it.
    pub(crate) fn begin_tree_order_walk(&mut self) {
        self.tree_order_walk = Some(Vec::new());
    }

    /// Lets the walk go on at an owner that does not follow the owners it resolved in tree order.
    pub(crate) fn restart_tree_order_walk(&mut self) {
        if let Some(walk) = &mut self.tree_order_walk {
            walk.clear();
        }
    }

    pub(crate) fn end_tree_order_walk(&mut self) {
        self.tree_order_walk = None;
    }

    /// The counters set of `owner`: its own if it has one, or else its parent element's in the flat tree.
    fn set_of(&self, engine: &RetainedState, owner: CounterOwner) -> Option<&CountersSet> {
        let mut owner = owner;
        loop {
            if let Some(set) = self.sets.get(&owner) {
                return Some(set);
            }
            owner = CounterOwner::element(parent_element(engine, owner)?);
        }
    }

    /// Gives `owner`, whose parent element in the flat tree has the set `parent_set`, the set `set`.
    fn store(&mut self, owner: CounterOwner, set: Option<CountersSet>, parent_set: Option<&CountersSet>) {
        let is_parent_set = match (&set, parent_set) {
            (None, None) => true,
            (Some(set), Some(parent_set)) => Arc::ptr_eq(set, parent_set) || **set == **parent_set,
            _ => false,
        };
        if is_parent_set {
            if self.sets.contains_key(&owner) {
                self.sets.remove(&owner);
            }
        } else {
            // NB: A set only ever extends its parent's, so only a root element's set can be empty, and an empty set is
            //     a root element's absent one.
            let set = set.expect("an owner with a parent element has its counters");
            self.insert(owner, set);
        }
    }

    /// Whether `owner` has a set of its own, apart from its parent element's.
    pub(crate) fn holds_set_of(&self, owner: CounterOwner) -> bool {
        let slot = owner.element.element_slot();
        let may_hold = self
            .may_hold_sets
            .get(slot / 64)
            .is_some_and(|word| word & (1 << (slot % 64)) != 0);
        may_hold && self.sets.contains_key(&owner)
    }

    /// Gives `owner` the set `set`, see [`Self::may_hold_sets`].
    fn insert(&mut self, owner: CounterOwner, set: CountersSet) {
        let slot = owner.element.element_slot();
        let may_hold_sets = Arc::make_mut(&mut self.may_hold_sets);
        if may_hold_sets.len() <= slot / 64 {
            may_hold_sets.resize(slot / 64 + 1, 0);
        }
        may_hold_sets[slot / 64] |= 1 << (slot % 64);
        self.sets.insert(owner, set);
    }

    /// Gives `owner` the set a build resolved for it, `set`, or for none, the set of its parent element, which the build
    /// found it has.
    pub(crate) fn store_resolved(&mut self, owner: CounterOwner, set: Option<CountersSet>) {
        match set {
            Some(set) => {
                self.insert(owner, set);
            }
            None => {
                if self.sets.contains_key(&owner) {
                    self.sets.remove(&owner);
                }
            }
        }
    }

    /// Drops every set, before a build that resolves the counters of every element and pseudo-element again stores
    /// what it resolved.
    pub(crate) fn forget_every_set(&mut self) {
        self.sets = Default::default();
        self.may_hold_sets = Default::default();
    }

    /// The counters `owner` held when it was last resolved, or those of its parent element.
    pub(crate) fn retained_counters_of(&self, engine: &RetainedState, owner: CounterOwner) -> &[Counter] {
        self.set_of(engine, owner).map_or(&[], |set| set.as_slice())
    }

    /// Drops the sets of an element and its pseudo-elements, once its identity is retired.
    pub(crate) fn forget(&mut self, element: StyleNodeID) {
        for owner in CounterOwner::every_owner_of(element) {
            self.sets.remove(&owner);
        }
    }

    fn list_item_counter_name(&mut self) -> CssString {
        self.list_item_counter_name
            .get_or_insert_with(|| CssString::from_utf16(&LIST_ITEM_COUNTER_NAME))
            .clone()
    }

    // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
    // "If there is not currently a counter of the given name on the element, the element instantiates
    // a new counter of the given name with a starting value of 0 before setting or incrementing its value."
    /// Returns every value named `name` in `owner`'s set, outermost first, instantiating the counter
    /// first when there is none.
    pub(crate) fn counter_values_for_use(
        &mut self,
        arena: &LayoutNodeArena,
        owner: CounterOwner,
        name: &CounterName<'_>,
    ) -> Vec<CounterValue> {
        arena.with_style_store(|engine| {
            if let Some(set) = self.set_of(engine, owner)
                && set.iter().any(|counter| name.matches(&counter.name))
            {
                return set
                    .iter()
                    .filter(|counter| name.matches(&counter.name))
                    .map(|counter| counter.value.unwrap_or(0))
                    .collect();
            }
            self.instantiate_unused_counter(engine, owner, name);
            vec![0]
        })
    }

    /// The value of the innermost counter named `name` in `owner`'s set, instantiating it first when
    /// there is none.
    pub(crate) fn counter_value_for_use(
        &mut self,
        arena: &LayoutNodeArena,
        owner: CounterOwner,
        name: &CounterName<'_>,
    ) -> CounterValue {
        arena.with_style_store(|engine| {
            if let Some(set) = self.set_of(engine, owner)
                && let Some(counter) = set.iter().rev().find(|counter| name.matches(&counter.name))
            {
                return counter.value.unwrap_or(0);
            }
            self.instantiate_unused_counter(engine, owner, name);
            0
        })
    }

    // NB: No counter of this name exists, so instantiating one never removes another and needs no
    //     tree order.
    fn instantiate_unused_counter(&mut self, engine: &RetainedState, owner: CounterOwner, name: &CounterName<'_>) {
        let name = name.to_css_string();
        let mut set = self.set_of(engine, owner).cloned().unwrap_or_default();
        Arc::make_mut(&mut set).push(Counter {
            name,
            originating_element: owner,
            reversed: false,
            value: Some(0),
        });
        // NB: The owners after it in a walk inherit the counter from its set as the walk holds it.
        if let Some(walk) = &mut self.tree_order_walk
            && let Some(entry) = walk.iter_mut().rev().find(|(walked, _)| *walked == owner)
        {
            entry.1 = Some(set.clone());
        }
        self.insert(owner, set);
    }
}

/// Whether an element with `style` has an innermost `list-item` counter of its own that counts forward: the last one its
/// `counter-reset` instantiates, which [`resolve_counters`] pushes after every counter the element inherits, unless the
/// element generates no box.
pub(crate) fn style_resets_forward_list_item_counter(style: ComputedValuesView<'_>) -> bool {
    !style.display().is_none()
        && style
            .counter_reset()
            .iter()
            .rev()
            .find(|counter| is_list_item_counter_name(counter.name().units()))
            .is_some_and(|counter| !counter.is_reversed())
}

/// The published style `owner` resolves its counters from. The style store settles no record for
/// `::backdrop`, so that one is read from the box it was built with.
pub(crate) fn style_of<'a>(
    arena: &'a LayoutNodeArena,
    engine: &'a RetainedState,
    owner: CounterOwner,
) -> Option<ComputedValuesView<'a>> {
    match owner.generated_for {
        0 => engine.published_style_view(owner.element, None),
        GENERATED_FOR_BACKDROP => {
            let row = arena.bound_pseudo_element_row(owner.element, GENERATED_FOR_BACKDROP);
            if row.is_invalid() {
                return None;
            }
            arena.style_view(row)
        }
        generated_for => engine.published_style_view(owner.element, Some(pseudo_kind_of(generated_for))),
    }
}

/// The element `owner` inherits counters from: a pseudo-element's originating element, or an element's parent in the
/// flat tree, which is the slot it is assigned to, its parent, or the host of the shadow root it is a child of. A
/// `display: contents` element is an element of this tree as any other.
fn parent_element(engine: &RetainedState, owner: CounterOwner) -> Option<StyleNodeID> {
    if owner.generated_for != 0 {
        return Some(owner.element);
    }
    // The document stands in the style tree, but is not an element.
    engine
        .inheritance_parent(owner.element)
        .filter(|&parent| !engine.is_relation_only(parent))
}

/// The owners among the children of `parent` in tree order before the flat tree child `child`, or before its
/// `::after` for none, nearest first. A pseudo-element counts where it is a child: `::marker` first, then `::before`,
/// then the element children, then `::after`. The flat tree children of an element are its shadow root's children, the
/// nodes assigned to it as a slot, or else its own children.
fn for_each_preceding_child(
    engine: &RetainedState,
    parent: StyleNodeID,
    child: Option<StyleNodeID>,
    mut visit: impl FnMut(CounterOwner) -> bool,
) {
    let assigned = if engine.element_is_slot(parent) {
        engine.assigned_nodes_of(parent)
    } else {
        &[]
    };
    let visited_every_sibling = if !assigned.is_empty() {
        let end = match child {
            Some(child) => assigned.iter().position(|&candidate| candidate == child).unwrap_or(0),
            None => assigned.len(),
        };
        !assigned[..end]
            .iter()
            .rev()
            .filter(|node| node.element_index().is_some())
            .any(|&sibling| visit(CounterOwner::element(sibling)))
    } else {
        let mut sibling = match child {
            Some(child) => engine.previous_sibling_in_dom_order(child),
            None => {
                let children_owner = engine.shadow_root_of(parent).unwrap_or(parent);
                engine.dom_children(children_owner).last()
            }
        };
        loop {
            let Some(current) = sibling else {
                break true;
            };
            if current.element_index().is_some() && visit(CounterOwner::element(current)) {
                break false;
            }
            sibling = engine.previous_sibling_in_dom_order(current);
        }
    };
    if !visited_every_sibling {
        return;
    }
    for generated_for in [GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER] {
        if visit(CounterOwner {
            element: parent,
            generated_for,
        }) {
            return;
        }
    }
}

/// The owners before `owner` among its siblings in tree order, nearest first, until `visit` answers true.
fn for_each_preceding_sibling(
    engine: &RetainedState,
    owner: CounterOwner,
    mut visit: impl FnMut(CounterOwner) -> bool,
) {
    match owner.generated_for {
        0 => {
            if let Some(parent) = parent_element(engine, owner) {
                for_each_preceding_child(engine, parent, Some(owner.element), visit);
            }
        }
        GENERATED_FOR_AFTER => for_each_preceding_child(engine, owner.element, None, visit),
        GENERATED_FOR_BEFORE => {
            visit(CounterOwner {
                element: owner.element,
                generated_for: GENERATED_FOR_MARKER,
            });
        }
        _ => {}
    }
}

/// Whether `owner` takes part in counter resolution: an element or pseudo-element that generates a box, or a
/// `display: contents` element. Any other passes over its counters properties, and a counters set it has is left from
/// an earlier build.
fn is_rendered(arena: &LayoutNodeArena, engine: &RetainedState, owner: CounterOwner) -> bool {
    if owner.generated_for != 0 {
        return !arena
            .bound_pseudo_element_row(owner.element, owner.generated_for)
            .is_invalid();
    }
    !arena.bound_row(owner.element).is_invalid()
        || engine
            .element_published_box_facts(owner.element)
            .is_some_and(|facts| facts.display.is_contents())
}

/// The nearest sibling before `owner` that takes part in counter resolution (see [`is_rendered`]).
fn nearest_rendered_preceding_sibling(
    arena: &LayoutNodeArena,
    engine: &RetainedState,
    owner: CounterOwner,
) -> Option<CounterOwner> {
    let mut found = None;
    for_each_preceding_sibling(engine, owner, |sibling| {
        if !is_rendered(arena, engine, sibling) {
            return false;
        }
        found = Some(sibling);
        true
    });
    found
}

/// The nearest owner before `child`, a flat tree child of `parent`, among the children of `parent` that take part in
/// counter resolution (see [`is_rendered`]): an element child or a pseudo-element of `parent`.
pub(crate) fn nearest_rendered_preceding_child(
    arena: &LayoutNodeArena,
    engine: &RetainedState,
    parent: StyleNodeID,
    child: StyleNodeID,
) -> Option<CounterOwner> {
    let mut found = None;
    for_each_preceding_child(engine, parent, Some(child), |owner| {
        if !is_rendered(arena, engine, owner) {
            return false;
        }
        found = Some(owner);
        true
    });
    found
}

/// The owner immediately preceding `owner` in tree order among those that take part in counter resolution: the deepest
/// last descendant of its nearest preceding sibling, or else its parent.
fn preceding_owner_in_tree_order(
    arena: &LayoutNodeArena,
    engine: &RetainedState,
    owner: CounterOwner,
) -> Option<CounterOwner> {
    let Some(mut current) = nearest_rendered_preceding_sibling(arena, engine, owner) else {
        return parent_element(engine, owner).map(CounterOwner::element);
    };
    // A pseudo-element has no children of its own.
    while current.generated_for == 0 {
        let after = CounterOwner {
            element: current.element,
            generated_for: GENERATED_FOR_AFTER,
        };
        let last_child = if is_rendered(arena, engine, after) {
            Some(after)
        } else {
            nearest_rendered_preceding_sibling(arena, engine, after)
        };
        let Some(last_child) = last_child else {
            break;
        };
        current = last_child;
    }
    Some(current)
}

fn last_counter_with_name<'a>(counters: &'a mut [Counter], name: &CssString) -> Option<&'a mut Counter> {
    counters.iter_mut().rev().find(|counter| counter.name == *name)
}

// https://drafts.csswg.org/css-lists-3/#instantiate-counter
fn instantiate_a_counter<'a>(
    engine: &RetainedState,
    counters: &'a mut Vec<Counter>,
    name: CssString,
    element: CounterOwner,
    reversed: bool,
    value: Option<CounterValue>,
) -> &'a mut Counter {
    // 1. Let counters be element’s CSS counters set.

    // 2. Let innermost counter be the last counter in counters with the name name.
    //    If innermost counter’s originating element is element or a previous sibling of element,
    //    remove innermost counter from counters.
    if let Some(innermost_counter) = counters.iter().rev().find(|counter| counter.name == name) {
        let innermost_element = innermost_counter.originating_element;
        // NB: A counter in element's set comes from before element in tree order, so a counter whose creator has the
        //     same parent is a previous sibling's.
        if innermost_element == element || parent_element(engine, innermost_element) == parent_element(engine, element)
        {
            let innermost_name = innermost_counter.name.clone();
            if let Some(index) = counters
                .iter()
                .position(|counter| counter.name == innermost_name && counter.originating_element == innermost_element)
            {
                counters.remove(index);
            }
        }
    }

    // 3. Append a new counter to counters with name name, originating element element,
    //    reversed being reversed, and initial value value (if given)
    counters.push(Counter {
        name,
        originating_element: element,
        reversed,
        value,
    });
    counters.last_mut().expect("the counter just appended")
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-set
fn set_a_counter(
    engine: &RetainedState,
    counters: &mut Vec<Counter>,
    name: &CssString,
    element: CounterOwner,
    value: CounterValue,
) {
    if let Some(existing_counter) = last_counter_with_name(counters, name) {
        existing_counter.value = Some(value);
        return;
    }

    // If there is not currently a counter of the given name on the element, the element instantiates
    // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
    // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
    let counter = instantiate_a_counter(engine, counters, name.clone(), element, false, Some(0));
    counter.value = Some(value);
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
fn increment_a_counter(
    engine: &RetainedState,
    counters: &mut Vec<Counter>,
    name: &CssString,
    element: CounterOwner,
    amount: CounterValue,
) {
    if let Some(existing_counter) = last_counter_with_name(counters, name) {
        let value = existing_counter.value.expect("an incremented counter has a value");
        existing_counter.value = Some(value.saturating_add(amount));
        return;
    }

    // If there is not currently a counter of the given name on the element, the element instantiates
    // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
    // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
    let counter = instantiate_a_counter(engine, counters, name.clone(), element, false, Some(0));
    counter.value = Some(CounterValue::saturating_add(0, amount));
}

// https://drafts.csswg.org/css-lists-3/#list-item-counter
// "Specifically, unless the counter-increment property explicitly specifies a different increment
// for the list-item counter, it must be incremented by 1 on every list item, or if the counter is
// reversed, it must be incremented by -1 on every list item instead, at the same time that counters
// are normally incremented (exactly as if the list item had list-item 1 or list-item -1 appended to
// their counter-increment value, including side-effects such as possibly instantiating a new
// counter, etc)."
pub(crate) fn style_has_implicit_list_item_increment(style: ComputedValuesView<'_>) -> bool {
    if !style.display().is_list_item() {
        return false;
    }
    !style
        .counter_increment()
        .iter()
        .any(|counter| is_list_item_counter_name(counter.name().units()))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ReversedScopeWalkDecision {
    Continue,
    Stop,
}

struct ReversedScopeWalkState<'a> {
    name: &'a CssString,
    name_is_list_item: bool,
    num: i64,
    last_nonzero_increment_negated: i64,
}

fn apply_reversed_counter_contribution(
    state: &mut ReversedScopeWalkState<'_>,
    style: ComputedValuesView<'_>,
) -> ReversedScopeWalkDecision {
    let mut increment: i64 = 0;
    for counter in style.counter_increment() {
        if counter.name() == state.name {
            increment += i64::from(counter.integer().expect("an increment has a value"));
        }
    }
    if state.name_is_list_item && style_has_implicit_list_item_increment(style) {
        increment = -1;
    }

    let increment_negated = -increment;
    if increment_negated != 0 {
        state.last_nonzero_increment_negated = increment_negated;
    }

    for counter in style.counter_set() {
        if counter.name() == state.name {
            state.num += i64::from(counter.integer().expect("a counter-set has a value"));
            return ReversedScopeWalkDecision::Stop;
        }
    }
    state.num += increment_negated;
    ReversedScopeWalkDecision::Continue
}

fn walk_reversed_counter_sibling_run(
    engine: &RetainedState,
    state: &mut ReversedScopeWalkState<'_>,
    first: Option<StyleNodeID>,
) -> ReversedScopeWalkDecision {
    let mut element = first;
    while let Some(current) = element {
        element = engine.next_element_sibling(current);
        let Some(style) = engine.published_style_view(current, None) else {
            continue;
        };
        if style.display().is_none() {
            continue;
        }
        if style.counter_reset().iter().any(|counter| counter.name() == state.name) {
            break;
        }
        if apply_reversed_counter_contribution(state, style) == ReversedScopeWalkDecision::Stop {
            return ReversedScopeWalkDecision::Stop;
        }
        if walk_reversed_counter_sibling_run(engine, state, engine.first_element_child(current))
            == ReversedScopeWalkDecision::Stop
        {
            return ReversedScopeWalkDecision::Stop;
        }
    }
    ReversedScopeWalkDecision::Continue
}

// https://drafts.csswg.org/css-lists-3/#instantiating-counters
// "When a counter is instantiated without an initial value, the user agent must dynamically
// calculate the initial value at layout-time to be the value returned by the following algorithm:
// 1. Let num be 0.
// 2. Let lastNonZeroIncrementNegated be 0.
// 3. For each element or pseudo-element el that increments or sets the same counter in the same scope:
//    1. Let incrementNegated be el's counter-increment integer value for this counter, multiplied by -1.
//    2. If incrementNegated is not zero, then set lastNonZeroIncrementNegated to incrementNegated.
//    3. If el sets this counter with counter-set, then add that integer value to num and break this loop.
//    4. Add incrementNegated to num.
// 4. Add lastNonZeroIncrementNegated to num.
// 5. Return num."
pub(crate) fn reversed_counter_start_value(
    engine: &RetainedState,
    name: &CssString,
    originating_element: CounterOwner,
    style: ComputedValuesView<'_>,
) -> CounterValue {
    let mut state = ReversedScopeWalkState {
        name,
        name_is_list_item: is_list_item_counter_name(name.units()),
        num: 0,
        last_nonzero_increment_negated: 0,
    };
    let mut decision = apply_reversed_counter_contribution(&mut state, style);
    // FIXME: Counters reset on pseudo-elements don't walk a scope yet; only the pseudo-element's own
    //        contribution is taken into account.
    if decision == ReversedScopeWalkDecision::Continue && originating_element.generated_for == 0 {
        let element = originating_element.element;
        decision = walk_reversed_counter_sibling_run(engine, &mut state, engine.first_element_child(element));
        if decision == ReversedScopeWalkDecision::Continue {
            walk_reversed_counter_sibling_run(engine, &mut state, engine.next_element_sibling(element));
        }
    }
    (state.num + state.last_nonzero_increment_negated).clamp(i64::from(CounterValue::MIN), i64::from(CounterValue::MAX))
        as CounterValue
}

/// What an owner inherits its counters from: the counters set of its parent element, and those of its nearest preceding
/// sibling and of the owner immediately preceding it in tree order, among the owners that take part in counter
/// resolution.
struct InheritedSets<ValueSource: FnOnce() -> Option<CountersSet>> {
    parent: Option<CountersSet>,
    sibling: Option<CountersSet>,
    value_source: ValueSource,
}

/// What `element`, whose parent element in the flat tree is `parent`, inherits its counters from, read from the sets
/// of the owners before it.
fn inherited_sets_in_tree<'a>(
    arena: &'a LayoutNodeArena,
    engine: &'a RetainedState,
    sets: &'a CountersSets,
    element: CounterOwner,
    parent: StyleNodeID,
) -> InheritedSets<impl FnOnce() -> Option<CountersSet> + 'a> {
    InheritedSets {
        parent: sets.set_of(engine, CounterOwner::element(parent)).cloned(),
        sibling: nearest_rendered_preceding_sibling(arena, engine, element)
            .and_then(|sibling| sets.set_of(engine, sibling).cloned()),
        value_source: move || {
            preceding_owner_in_tree_order(arena, engine, element)
                .and_then(|previous| sets.set_of(engine, previous).cloned())
        },
    }
}

// https://drafts.csswg.org/css-lists-3/#inherit-counters
// NB: The tree is the flat element tree, pseudo-elements included, as in the rest of the algorithm. An element or
//     pseudo-element that generates no box, and is not a `display: contents` element, is passed over. The caller
//     returns for the root of its document tree (step 1), and finds the sets steps 2 to 4 read.
fn inherit_counters(inherited: InheritedSets<impl FnOnce() -> Option<CountersSet>>) -> Option<CountersSet> {
    // 1. If element is the root of its document tree, the element has an initially-empty CSS counters set.
    //    Return.

    // 2. Let element counters, representing element’s own CSS counters set, be a copy of the CSS counters
    //    set of element’s parent element.
    let mut element_counters = inherited.parent;

    // 3. Let sibling counters be the CSS counters set of element’s preceding sibling (if it has one),
    //    or an empty CSS counters set otherwise.
    //    For each counter of sibling counters, if element counters does not already contain a counter with
    //    the same name, append a copy of counter to element counters.
    if let Some(sibling_counters) = inherited.sibling {
        let element_counters = element_counters.get_or_insert_with(Default::default);
        for counter in sibling_counters.iter() {
            if !element_counters.iter().any(|existing| existing.name == counter.name) {
                Arc::make_mut(element_counters).push(counter.clone());
            }
        }
    }

    // 4. Let value source be the CSS counters set of the element immediately preceding element in tree order.
    //    For each source counter of value source, if element counters contains a counter with the same name
    //    and creator, then set the value of that counter to source counter’s value.
    // NB: If element counters is empty then we can skip this since nothing will match.
    if let Some(element_counters) = element_counters.as_mut()
        && let Some(value_source) = (inherited.value_source)()
        && !Arc::ptr_eq(&value_source, element_counters)
    {
        for source_counter in value_source.iter() {
            let existing = element_counters.iter().position(|counter| {
                counter.name == source_counter.name && counter.originating_element == source_counter.originating_element
            });
            if let Some(index) = existing
                && element_counters[index].value != source_counter.value
            {
                Arc::make_mut(element_counters)[index].value = source_counter.value;
            }
        }
    }

    element_counters
}

/// The counters `element` inherits, and the set of its parent element, which a set that is the same is not stored
/// apart from. A walk in tree order reads what it inherits from the owners it resolved before it.
fn inherit_counters_of(
    arena: &LayoutNodeArena,
    engine: &RetainedState,
    sets: &mut CountersSets,
    element: CounterOwner,
) -> (Option<CountersSet>, Option<CountersSet>) {
    let Some(parent) = parent_element(engine, element) else {
        if let Some(walk) = &mut sets.tree_order_walk {
            walk.clear();
        }
        return (None, None);
    };
    let parent_owner = CounterOwner::element(parent);
    // NB: The parent is open in the walk unless the walk's span starts below it, which the walk then notes. The child
    //     of the parent after it in the walk is the owner's nearest preceding sibling, and the walk's last owner the
    //     owner immediately preceding it. A top layer member, which the walk resolves after the document, is the one
    //     sibling it passes over, as its place in tree order says it inherits what the owners before it hold.
    // NB: A ::backdrop is no child of its element in tree order, but follows it as a box of the top layer.
    if let Some(walk) = &mut sets.tree_order_walk
        && element.generated_for != GENERATED_FOR_BACKDROP
        && let Some(position) = walk.iter().rposition(|(walked, _)| *walked == parent_owner)
    {
        let value_source = walk.last().and_then(|(_, set)| set.clone());
        let inherited = InheritedSets {
            parent: walk[position].1.clone(),
            sibling: walk.get(position + 1).and_then(|(_, set)| set.clone()),
            value_source: || value_source,
        };
        walk.truncate(position + 1);
        let parent_set = inherited.parent.clone();
        return (inherit_counters(inherited), parent_set);
    }
    let inherited = inherited_sets_in_tree(arena, engine, sets, element, parent);
    let parent_set = inherited.parent.clone();
    let counters = inherit_counters(inherited);
    if let Some(walk) = &mut sets.tree_order_walk {
        walk.clear();
        walk.push((parent_owner, parent_set.clone()));
    }
    (counters, parent_set)
}

/// What `owner` inherits from the owners before it in tree order as the builds before resolved them: the counters of the
/// set of its parent element, then those of the set of its nearest preceding sibling it adds, each with the value of the
/// owner immediately preceding it in tree order. Answers them with how many come from its parent element's set, and that
/// set.
pub(crate) fn retained_inherited_counters(
    arena: &LayoutNodeArena,
    engine: &RetainedState,
    sets: &CountersSets,
    owner: CounterOwner,
) -> (Vec<Counter>, usize, Option<CountersSet>) {
    let Some(parent) = parent_element(engine, owner) else {
        return (Vec::new(), 0, None);
    };
    let inherited = inherited_sets_in_tree(arena, engine, sets, owner, parent);
    let parent_set = inherited.parent.clone();
    let parent_count = parent_set.as_ref().map_or(0, |set| set.len());
    let counters = inherit_counters(inherited).map_or_else(Vec::new, |set| (*set).clone());
    (counters, parent_count, parent_set)
}

/// The owner last in tree order among `element` and the owners below it that take part in counter resolution, as the
/// boxes the builds before made have them.
pub(crate) fn last_owner_in_tree_order_within(
    arena: &LayoutNodeArena,
    engine: &RetainedState,
    element: StyleNodeID,
) -> CounterOwner {
    let mut current = CounterOwner::element(element);
    // A pseudo-element has no children of its own.
    while current.generated_for == 0 {
        let after = CounterOwner {
            element: current.element,
            generated_for: GENERATED_FOR_AFTER,
        };
        let last_child = if is_rendered(arena, engine, after) {
            Some(after)
        } else {
            nearest_rendered_preceding_sibling(arena, engine, after)
        };
        let Some(last_child) = last_child else {
            break;
        };
        current = last_child;
    }
    current
}

// https://drafts.csswg.org/css-lists-3/#auto-numbering
pub(crate) fn resolve_counters(arena: &mut LayoutNodeArena, element: CounterOwner) {
    arena.with_counter_tables_lent(|arena, tables| resolve_counters_into(arena, &mut tables.counters_sets, element));
}

fn resolve_counters_into(arena: &LayoutNodeArena, sets: &mut CountersSets, element: CounterOwner) {
    arena.with_style_store(|engine| {
        // Resolving counter values on a given element is a multi-step process:
        let style = style_of(arena, engine, element).expect("an element that resolves counters has a style");

        // 1. Existing counters are inherited from previous elements.
        // OPTIMIZATION: No element has a counter yet, so there is none to inherit.
        let (mut counters, parent_set) = if sets.sets.is_empty() {
            if let Some(walk) = &mut sets.tree_order_walk {
                walk.clear();
            }
            (None, None)
        } else {
            inherit_counters_of(arena, engine, sets, element)
        };

        // https://drafts.csswg.org/css-lists-3/#counters-without-boxes
        // An element that does not generate a box (for example, an element with display set to none,
        // or a pseudo-element with content set to none) cannot set, reset, or increment a counter.
        // The counter properties are still valid on such an element, but they must have no effect.
        if !style.display().is_none() {
            // 2. New counters are instantiated (counter-reset).
            for counter in style.counter_reset() {
                let mut value = counter.integer();
                if counter.is_reversed() && value.is_none() {
                    value = Some(reversed_counter_start_value(engine, counter.name(), element, style));
                }
                instantiate_a_counter(
                    engine,
                    Arc::make_mut(counters.get_or_insert_with(Default::default)),
                    counter.name().clone(),
                    element,
                    counter.is_reversed(),
                    value,
                );
            }

            // FIXME: Take style containment into account
            // https://drafts.csswg.org/css-contain-2/#containment-style
            // Giving an element style containment has the following effects:
            // 1. The 'counter-increment' and 'counter-set' properties must be scoped to the element’s sub-tree and create a
            //    new counter.

            // 3. Counter values are incremented (counter-increment).
            for counter in style.counter_increment() {
                increment_a_counter(
                    engine,
                    Arc::make_mut(counters.get_or_insert_with(Default::default)),
                    counter.name(),
                    element,
                    counter.integer().expect("an increment has a value"),
                );
            }

            if style_has_implicit_list_item_increment(style) {
                let list_item_counter_name = sets.list_item_counter_name();
                let counters = Arc::make_mut(counters.get_or_insert_with(Default::default));
                let reversed = counters
                    .iter()
                    .rev()
                    .find(|counter| counter.name == list_item_counter_name)
                    .is_some_and(|counter| counter.reversed);
                increment_a_counter(
                    engine,
                    counters,
                    &list_item_counter_name,
                    element,
                    if reversed { -1 } else { 1 },
                );
            }

            // 4. Counter values are explicitly set (counter-set).
            for counter in style.counter_set() {
                set_a_counter(
                    engine,
                    Arc::make_mut(counters.get_or_insert_with(Default::default)),
                    counter.name(),
                    element,
                    counter.integer().expect("a counter-set has a value"),
                );
            }

            // 5. Counter values are used (counter()/counters()).
            // NOTE: This happens when we process the `content` property.
        }

        if let Some(walk) = &mut sets.tree_order_walk {
            walk.push((element, counters.clone()));
        }
        sets.store(element, counters, parent_set.as_ref());
    });
}
