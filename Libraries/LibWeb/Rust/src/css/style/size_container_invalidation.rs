/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Recording the elements whose style a size query container's new box moves.

use super::fast_hash::FastSet as HashSet;
use super::{RetainedState, StyleEngine, StyleNodeID};

/// What the host learned about size container queries while it computed styles: which elements
/// were asked about, which elements asked, and which containers had no box to answer with yet.
#[derive(Clone, Default)]
pub(super) struct SizeContainerQueryFacts {
    /// Elements some size query or container-relative unit resolved against. `container-type` is
    /// set far more widely than it is asked about, so a container outside this set has no
    /// dependent below it to find.
    queried_containers: HashSet<StyleNodeID>,
    /// Elements whose style some size query or container-relative unit decided.
    dependents: HashSet<StyleNodeID>,
    /// Containers a style computation asked about before they had a committed box. The layout
    /// that gives them one is where their dependents move.
    needing_evaluation_after_layout: HashSet<StyleNodeID>,
    /// A container's eligibility changed, so units can newly select it even when it was not queried before.
    availability_changed: HashSet<StyleNodeID>,
    /// Elements the dependent walks have visited, for the style invalidation counters.
    scan_visits: u64,
}

impl SizeContainerQueryFacts {
    pub(super) fn is_queried_container(&self, node: StyleNodeID) -> bool {
        self.queried_containers.contains(&node)
    }

    pub(super) fn note_font_input_change(&mut self, node: StyleNodeID) {
        self.needing_evaluation_after_layout.insert(node);
    }

    pub(super) fn note_availability_change(&mut self, node: StyleNodeID) {
        self.availability_changed.insert(node);
    }

    pub(super) fn retire(&mut self, node: StyleNodeID) {
        self.queried_containers.remove(&node);
        self.dependents.remove(&node);
        self.needing_evaluation_after_layout.remove(&node);
        self.availability_changed.remove(&node);
    }
}

fn set_contains(set: &mut HashSet<StyleNodeID>, node: StyleNodeID, contains: bool) {
    if contains {
        set.insert(node);
    } else {
        set.remove(&node);
    }
}

impl StyleEngine {
    /// The host's two facts about an element: whether a size query or container-relative unit
    /// resolved against it, and whether one decided its style.
    pub fn set_element_size_container_query_facts(
        &mut self,
        node: StyleNodeID,
        is_queried_container: bool,
        depends_on_size_container_query: bool,
    ) {
        let facts = &mut self.retained.size_container_queries;
        set_contains(&mut facts.queried_containers, node, is_queried_container);
        set_contains(&mut facts.dependents, node, depends_on_size_container_query);
    }

    pub fn note_size_container_needs_evaluation_after_layout(&mut self, node: StyleNodeID) {
        self.retained
            .size_container_queries
            .needing_evaluation_after_layout
            .insert(node);
    }

    #[must_use]
    pub fn has_size_containers_needing_evaluation_after_layout(&self) -> bool {
        !self
            .retained
            .size_container_queries
            .needing_evaluation_after_layout
            .is_empty()
            || !self.retained.size_container_queries.availability_changed.is_empty()
    }

    /// The elements the dependent walks have visited, optionally starting the count again.
    pub fn size_query_container_scan_visits(&mut self, reset: bool) -> u64 {
        let visits = &mut self.retained.size_container_queries.scan_visits;
        if reset { std::mem::take(visits) } else { *visits }
    }

    /// A layout gave the containers that had no box when they were asked about one; their
    /// dependents are computed again against it.
    pub fn evaluate_size_containers_needing_evaluation_after_layout(&mut self) {
        let mut containers = std::mem::take(&mut self.retained.size_container_queries.needing_evaluation_after_layout);
        let availability_changed = std::mem::take(&mut self.retained.size_container_queries.availability_changed);
        containers.extend(availability_changed.iter().copied());
        for container in containers {
            // A container that has left the document has no dependents left to move.
            if self.retained.tree.is_live(container) {
                self.record_size_container_query_dependents_after_input_change(
                    container,
                    availability_changed.contains(&container),
                );
            }
        }
    }

    /// What `container` answers to the queries below it changed: its content box moved along an
    /// axis its container type queries, or its scroll state moved. Every element whose style a
    /// size query or container-relative unit decided below it, and the container itself for its
    /// own pseudo-elements, is recorded to compute again.
    pub fn record_size_container_query_dependents(&mut self, container: StyleNodeID) {
        self.record_size_container_query_dependents_after_input_change(container, false);
    }

    fn record_size_container_query_dependents_after_input_change(
        &mut self,
        container: StyleNodeID,
        availability_changed: bool,
    ) {
        let (changed, visits) = if availability_changed {
            self.retained.size_container_query_dependents_in_subtree(container)
        } else {
            self.retained.size_container_query_dependents(container)
        };
        self.retained.size_container_queries.scan_visits += visits;
        for node in changed {
            self.record_container_query_input(node);
        }
    }
}

impl RetainedState {
    /// The elements whose style a size query or container-relative unit decided below `container`, and the container
    /// itself for its own pseudo-elements, with the number of elements the walk for them visited. None where nothing
    /// resolved against the container.
    pub(super) fn size_container_query_dependents(&self, container: StyleNodeID) -> (Vec<StyleNodeID>, u64) {
        if !self.size_container_queries.queried_containers.contains(&container) {
            return (Vec::new(), 0);
        }
        self.size_container_query_dependents_in_subtree(container)
    }

    fn size_container_query_dependents_in_subtree(&self, container: StyleNodeID) -> (Vec<StyleNodeID>, u64) {
        let facts = &self.size_container_queries;
        if facts.dependents.is_empty() {
            return (Vec::new(), 0);
        }
        let mut changed = Vec::new();
        // The container's own pseudo-elements select it as their query container too, and their
        // styles are computed again with the element's.
        if facts.dependents.contains(&container) {
            changed.push(container);
        }

        // The flat tree below the container is the inverse of the walk that selects a query
        // container, which is what makes it the right one for finding that container's dependents.
        let mut visits = 0;
        let mut stack: Vec<StyleNodeID> = self.tree.flat_tree_children(container).collect();
        while let Some(node) = stack.pop() {
            // A text node holds a place among a slot's assigned nodes, but has no style of its own.
            if node.text_index().is_some() {
                continue;
            }
            visits += 1;
            if facts.dependents.contains(&node) {
                changed.push(node);
            }
            stack.extend(self.tree.flat_tree_children(node));
        }
        (changed, visits)
    }
}
