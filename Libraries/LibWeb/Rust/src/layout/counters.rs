/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS counters sets the layout tree build resolves, one per element and pseudo-element that
//! keeps a box, and the reads that generated content and list markers make of them.

use super::layout_node_arena::LayoutNodeArena;
use super::node_data::{
    GENERATED_FOR_AFTER, GENERATED_FOR_BACKDROP, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, NodeSlotId, pseudo_kind_of,
};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_string::CssString;
use crate::css::style::StyleEngine;
use crate::css::style::fast_hash::FastMap as HashMap;
use crate::css::style::tree::StyleNodeID;
use crate::painting::paint_read::PaintRead;
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

fn is_list_item_counter_name(name: &[u16]) -> bool {
    name == LIST_ITEM_COUNTER_NAME
}

/// A counter name as a reader of the counters set has it.
pub(crate) enum CounterName<'a> {
    /// A name a computed style spells.
    Css(&'a CssString),
    Units(&'a [u16]),
}

impl CounterName<'_> {
    fn matches(&self, counter_name: &CssString) -> bool {
        let counter_name = counter_name.units();
        match self {
            Self::Css(name) => name.units() == counter_name,
            Self::Units(units) => *units == counter_name,
        }
    }

    fn to_css_string(&self) -> CssString {
        match self {
            Self::Css(name) => (*name).clone(),
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
#[derive(Clone)]
struct Counter {
    name: CssString,
    originating_element: CounterOwner, // "creator"
    reversed: bool,
    value: Option<CounterValue>,
}

// https://drafts.csswg.org/css-lists-3/#css-counters-set
// NB: Most elements inherit their parent's set unchanged, so a set is shared until it is written.
type CountersSet = Arc<Vec<Counter>>;

/// Every non-empty counters set, keyed by its owner. An empty set is simply absent.
#[derive(Default)]
pub(crate) struct CountersSets {
    sets: HashMap<CounterOwner, CountersSet>,
    list_item_counter_name: Option<CssString>,
}

impl CountersSets {
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
    pub(crate) fn counter_values_for_use(&mut self, owner: CounterOwner, name: &CounterName<'_>) -> Vec<CounterValue> {
        if let Some(set) = self.sets.get(&owner)
            && set.iter().any(|counter| name.matches(&counter.name))
        {
            return set
                .iter()
                .filter(|counter| name.matches(&counter.name))
                .map(|counter| counter.value.unwrap_or(0))
                .collect();
        }
        self.instantiate_unused_counter(owner, name);
        vec![0]
    }

    /// The value of the innermost counter named `name` in `owner`'s set, instantiating it first when
    /// there is none.
    pub(crate) fn counter_value_for_use(&mut self, owner: CounterOwner, name: &CounterName<'_>) -> CounterValue {
        if let Some(set) = self.sets.get(&owner)
            && let Some(counter) = set.iter().rev().find(|counter| name.matches(&counter.name))
        {
            return counter.value.unwrap_or(0);
        }
        self.instantiate_unused_counter(owner, name);
        0
    }

    // NB: No counter of this name exists, so instantiating one never removes another and needs no
    //     tree order.
    fn instantiate_unused_counter(&mut self, owner: CounterOwner, name: &CounterName<'_>) {
        let name = name.to_css_string();
        Arc::make_mut(self.sets.entry(owner).or_default()).push(Counter {
            name,
            originating_element: owner,
            reversed: false,
            value: Some(0),
        });
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
    engine: &'a StyleEngine,
    owner: CounterOwner,
) -> Option<ComputedValuesView<'a>> {
    match owner.generated_for {
        0 => engine.published_style_view(owner.element, None),
        GENERATED_FOR_BACKDROP => {
            let row = arena.bound_pseudo_element_row(owner.element, GENERATED_FOR_BACKDROP);
            if row.is_invalid() {
                return None;
            }
            arena
                .style_payloads(row)
                .map(|payloads| ComputedValuesView::new(&payloads.groups))
        }
        generated_for => engine.published_style_view(owner.element, Some(pseudo_kind_of(generated_for))),
    }
}

/// The element's parent element, or the originating element of a pseudo-element.
fn parent_element(engine: &StyleEngine, owner: CounterOwner) -> Option<StyleNodeID> {
    if owner.generated_for != 0 {
        return Some(owner.element);
    }
    let tree = engine.tree();
    let parent = tree.parent(owner.element)?;
    // A shadow root and the document stand in the style tree, but neither is an element.
    if tree.host_of(parent).is_some() || tree.is_relation_only(parent) {
        return None;
    }
    Some(parent)
}

fn layout_node_of(arena: &LayoutNodeArena, owner: CounterOwner) -> NodeSlotId {
    if owner.generated_for == 0 {
        arena.bound_row(owner.element)
    } else {
        arena.bound_pseudo_element_row(owner.element, owner.generated_for)
    }
}

fn next_in_pre_order(arena: &LayoutNodeArena, node: NodeSlotId) -> NodeSlotId {
    let first_child = arena.data(node).first_child.get();
    if !first_child.is_invalid() {
        return first_child;
    }
    let mut current = node;
    loop {
        let data = arena.data(current);
        let next_sibling = data.next_sibling.get();
        if !next_sibling.is_invalid() {
            return next_sibling;
        }
        current = data.parent.get();
        if current.is_invalid() {
            return NodeSlotId::INVALID;
        }
    }
}

/// Whether `owner`'s box comes before `other`'s in layout tree order. Either one having no box answers no.
fn is_before(arena: &LayoutNodeArena, owner: CounterOwner, other: CounterOwner) -> bool {
    let node = layout_node_of(arena, owner);
    let other = layout_node_of(arena, other);
    if node.is_invalid() || other.is_invalid() || node == other {
        return false;
    }
    let mut current = next_in_pre_order(arena, node);
    while !current.is_invalid() {
        if current == other {
            return true;
        }
        current = next_in_pre_order(arena, current);
    }
    false
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum WalkMethod {
    Previous,
    PreviousSibling,
}

/// The element or tree-abiding pseudo-element whose box precedes `owner`'s in the layout tree.
fn walk_layout_tree(arena: &LayoutNodeArena, owner: CounterOwner, walk_method: WalkMethod) -> Option<CounterOwner> {
    let mut node = layout_node_of(arena, owner);
    if node.is_invalid() {
        return None;
    }
    // A row kept after its element or its pseudo-element's generator was removed names neither, and is skipped.
    loop {
        node = arena.previous_dom_backed_or_generated_node(node, walk_method == WalkMethod::PreviousSibling);
        if node.is_invalid() {
            return None;
        }
        if arena.node_is_element_backed(node) {
            if let Some(element) = arena.node_style_node(node) {
                return Some(CounterOwner::element(element));
            }
            continue;
        }
        let generated_for = arena.data(node).generated_for.get();
        if matches!(
            generated_for,
            GENERATED_FOR_AFTER | GENERATED_FOR_BEFORE | GENERATED_FOR_MARKER
        ) && let Some(element) = arena.node_style_node(node)
        {
            return Some(CounterOwner { element, generated_for });
        }
    }
}

fn last_counter_with_name<'a>(counters: &'a mut [Counter], name: &CssString) -> Option<&'a mut Counter> {
    counters.iter_mut().rev().find(|counter| counter.name == *name)
}

// https://drafts.csswg.org/css-lists-3/#instantiate-counter
fn instantiate_a_counter<'a>(
    arena: &LayoutNodeArena,
    engine: &StyleEngine,
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
        if innermost_element == element
            || (parent_element(engine, innermost_element) == parent_element(engine, element)
                && is_before(arena, innermost_element, element))
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
    arena: &LayoutNodeArena,
    engine: &StyleEngine,
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
    let counter = instantiate_a_counter(arena, engine, counters, name.clone(), element, false, Some(0));
    counter.value = Some(value);
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
fn increment_a_counter(
    arena: &LayoutNodeArena,
    engine: &StyleEngine,
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
    let counter = instantiate_a_counter(arena, engine, counters, name.clone(), element, false, Some(0));
    counter.value = Some(CounterValue::saturating_add(0, amount));
}

// https://drafts.csswg.org/css-lists-3/#list-item-counter
// "Specifically, unless the counter-increment property explicitly specifies a different increment
// for the list-item counter, it must be incremented by 1 on every list item, or if the counter is
// reversed, it must be incremented by -1 on every list item instead, at the same time that counters
// are normally incremented (exactly as if the list item had list-item 1 or list-item -1 appended to
// their counter-increment value, including side-effects such as possibly instantiating a new
// counter, etc)."
fn style_has_implicit_list_item_increment(style: ComputedValuesView<'_>) -> bool {
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
    engine: &StyleEngine,
    state: &mut ReversedScopeWalkState<'_>,
    first: Option<StyleNodeID>,
) -> ReversedScopeWalkDecision {
    let tree = engine.tree();
    let mut element = first;
    while let Some(current) = element {
        element = tree.next_element_sibling(current);
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
        if walk_reversed_counter_sibling_run(engine, state, tree.first_element_child(current))
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
fn reversed_counter_start_value(
    engine: &StyleEngine,
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
        let tree = engine.tree();
        let element = originating_element.element;
        decision = walk_reversed_counter_sibling_run(engine, &mut state, tree.first_element_child(element));
        if decision == ReversedScopeWalkDecision::Continue {
            walk_reversed_counter_sibling_run(engine, &mut state, tree.next_element_sibling(element));
        }
    }
    (state.num + state.last_nonzero_increment_negated).clamp(i64::from(CounterValue::MIN), i64::from(CounterValue::MAX))
        as CounterValue
}

// https://drafts.csswg.org/css-lists-3/#inherit-counters
fn inherit_counters(
    arena: &LayoutNodeArena,
    engine: &StyleEngine,
    sets: &CountersSets,
    element: CounterOwner,
) -> Option<CountersSet> {
    // 1. If element is the root of its document tree, the element has an initially-empty CSS counters set.
    //    Return.
    let parent = parent_element(engine, element)?;

    // 2. Let element counters, representing element’s own CSS counters set, be a copy of the CSS counters
    //    set of element’s parent element.
    let mut element_counters = sets.sets.get(&CounterOwner::element(parent)).cloned();

    // 3. Let sibling counters be the CSS counters set of element’s preceding sibling (if it has one),
    //    or an empty CSS counters set otherwise.
    //    For each counter of sibling counters, if element counters does not already contain a counter with
    //    the same name, append a copy of counter to element counters.
    if let Some(sibling) = walk_layout_tree(arena, element, WalkMethod::PreviousSibling)
        && let Some(sibling_counters) = sets.sets.get(&sibling)
    {
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
        && let Some(previous) = walk_layout_tree(arena, element, WalkMethod::Previous)
        && let Some(value_source) = sets.sets.get(&previous)
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

// https://drafts.csswg.org/css-lists-3/#auto-numbering
pub(crate) fn resolve_counters(arena: &LayoutNodeArena, element: CounterOwner) {
    arena.with_style_store(|engine| {
        // Resolving counter values on a given element is a multi-step process:
        let style = style_of(arena, engine, element).expect("an element that resolves counters has a style");

        // 1. Existing counters are inherited from previous elements.
        let mut sets = arena.counters_sets().borrow_mut();
        let mut counters = inherit_counters(arena, engine, &sets, element);

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
                    arena,
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
                    arena,
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
                    arena,
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
                    arena,
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

        match counters {
            Some(counters) => {
                assert!(!counters.is_empty());
                sets.sets.insert(element, counters);
            }
            None => {
                sets.sets.remove(&element);
            }
        }
    });
}
