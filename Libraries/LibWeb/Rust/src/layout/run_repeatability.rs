/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! With LIBWEB_VERIFY_RUN_REPEATABILITY=cold, every layout run that executes is laid out twice. The first layout reads
//! no memoized layout from before it, keeps none after it, and its result is discarded; the second is the run itself.
//! Both have to agree on everything the run hands its parent, which shows that a run depends on nothing but its input
//! and the state of the pass, and that what it reads from memoized layout is what it would have computed.

use super::*;

#[derive(Clone, Copy, PartialEq, Eq)]
enum RunRepeatabilityVerification {
    Off,
    Cold,
}

fn run_repeatability_verification() -> RunRepeatabilityVerification {
    static VERIFICATION: std::sync::OnceLock<RunRepeatabilityVerification> = std::sync::OnceLock::new();
    *VERIFICATION.get_or_init(|| match std::env::var("LIBWEB_VERIFY_RUN_REPEATABILITY").as_deref() {
        Err(_) => RunRepeatabilityVerification::Off,
        Ok("cold") => RunRepeatabilityVerification::Cold,
        Ok(other) => panic!("LIBWEB_VERIFY_RUN_REPEATABILITY must be cold, not {other:?}"),
    })
}

pub(super) fn verification_is_enabled() -> bool {
    run_repeatability_verification() != RunRepeatabilityVerification::Off
}

/// Lays out a run whose result is discarded through `run`, where the verification is on and no such run is in
/// progress around it already. Nothing it does is traced.
pub(super) fn lay_out_discarded_run(
    callbacks: &LayoutPass<'_>,
    run: impl FnOnce() -> formatting_context::RunOutputs,
) -> Option<formatting_context::RunOutputs> {
    if run_repeatability_verification() == RunRepeatabilityVerification::Off
        || callbacks.scratch().reads_no_memoized_layout()
    {
        return None;
    }
    Some(
        callbacks
            .arena()
            .layout_trace
            .without_events(|| callbacks.scratch().lay_out_discarded_run(run)),
    )
}

/// Checks that the run of `box_` that produced `used` repeated what the discarded run that produced `discarded` did.
pub(super) fn verify_repeated_run(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    discarded: &formatting_context::RunOutputs,
    used: &formatting_context::RunOutputs,
) {
    assert_eq!(
        used.result, discarded.result,
        "the run of {box_:?} returned a result its discarded repetition does not"
    );
    assert_eq!(
        used.root_outcome.cells, discarded.root_outcome.cells,
        "the run of {box_:?} left a root record its discarded repetition does not"
    );
    assert_eq!(
        used.root_outcome.own_metrics_sealed, discarded.root_outcome.own_metrics_sealed,
        "the run of {box_:?} seals the root's metrics unlike its discarded repetition"
    );
    assert!(
        used.root_outcome.line_data.as_deref() == discarded.root_outcome.line_data.as_deref(),
        "the run of {box_:?} left line data its discarded repetition does not"
    );
    assert!(
        rare_payloads_match(
            used.root_outcome.rare.as_deref(),
            discarded.root_outcome.rare.as_deref()
        ),
        "the run of {box_:?} left rare payloads its discarded repetition does not"
    );
    assert_eq!(
        used.atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
        discarded.atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
        "the run of {box_:?} claims an available inline size threshold its discarded repetition does not"
    );
    assert!(
        host_findings_match(used.host_findings.as_deref(), discarded.host_findings.as_deref()),
        "the run of {box_:?} found out for the host what its discarded repetition does not"
    );
    let (used_root, discarded_root) = match (&used.root, &discarded.root) {
        (None, None) => return,
        (Some(used_root), Some(discarded_root)) => (used_root, discarded_root),
        _ => panic!("the run of {box_:?} and its discarded repetition do not both build fragments"),
    };
    assert!(
        discarded_root.reused_subtree_roots.is_empty(),
        "the discarded repetition of the run of {box_:?} reused a subtree"
    );
    assert!(
        same_elements_in_any_order(
            &used_root.propagated_pending_abspos,
            &discarded_root.propagated_pending_abspos
        ) && same_elements_in_any_order(
            &used_root.propagated_anchor_candidates,
            &discarded_root.propagated_anchor_candidates
        ) && same_elements_in_any_order(
            &used_root.propagated_inline_containing_block_rects,
            &discarded_root.propagated_inline_containing_block_rects
        ) && same_elements_in_any_order(
            &used_root.propagated_abspos_containing_block_info,
            &discarded_root.propagated_abspos_containing_block_info
        ),
        "the run of {box_:?} propagates payloads its discarded repetition does not"
    );
    let mut nodes_laid_out_afresh = HashSet::default();
    collect_nodes_of_fragment_subtrees(&discarded_root.scoped_descendants, &mut nodes_laid_out_afresh);
    let comparison = FreshDescendantComparison {
        callbacks,
        reused_subtree_roots: &used_root.reused_subtree_roots,
        nodes_laid_out_afresh,
    };
    if let Err(difference) = comparison.compare_children(
        box_,
        &used_root.scoped_descendants,
        &discarded_root.scoped_descendants,
        false,
    ) {
        panic!("the run of {box_:?} lays out descendants unlike its discarded repetition: {difference}");
    }
}

fn same_elements_in_any_order<T: PartialEq>(left: &[T], right: &[T]) -> bool {
    if left.len() != right.len() {
        return false;
    }
    let mut matched = vec![false; right.len()];
    left.iter().all(|element| {
        let Some(index) = (0..right.len()).find(|&index| !matched[index] && right[index] == *element) else {
            return false;
        };
        matched[index] = true;
        true
    })
}

fn host_findings_match(left: Option<&run_records::HostFindings>, right: Option<&run_records::HostFindings>) -> bool {
    let none = run_records::HostFindings::default();
    let left = left.unwrap_or(&none);
    let right = right.unwrap_or(&none);
    same_elements_in_any_order(
        &left.nodes_with_unresolved_container_lengths,
        &right.nodes_with_unresolved_container_lengths,
    ) && same_elements_in_any_order(&left.messages, &right.messages)
        && left.document_svg_root_natural_size == right.document_svg_root_natural_size
}

fn rare_payloads_match(
    left: Option<&used_values::UsedValuesRareData>,
    right: Option<&used_values::UsedValuesRareData>,
) -> bool {
    let none = used_values::UsedValuesRareData::default();
    let left = left.unwrap_or(&none);
    let right = right.unwrap_or(&none);
    let used_values::UsedValuesRareData {
        computed_svg_path,
        svg_text_character_cells,
        svg,
        grid_layout_data,
        flex_layout_data,
        multicol_layout_data,
        math_paint_data,
        used_grid_tracks,
        collapsed_table_borders,
        abspos_layout_inputs,
        default_scroll_shift,
        orthogonal_flow_baselines,
    } = left;
    computed_svg_path.as_deref() == right.computed_svg_path.as_deref()
        && svg_text_character_cells.as_deref() == right.svg_text_character_cells.as_deref()
        && *svg == right.svg
        && grid_layout_data.as_deref() == right.grid_layout_data.as_deref()
        && flex_layout_data.as_deref() == right.flex_layout_data.as_deref()
        && multicol_layout_data.as_deref() == right.multicol_layout_data.as_deref()
        && math_paint_data.as_deref() == right.math_paint_data.as_deref()
        && used_grid_tracks.as_deref() == right.used_grid_tracks.as_deref()
        && collapsed_table_borders.as_deref() == right.collapsed_table_borders.as_deref()
        && *abspos_layout_inputs == right.abspos_layout_inputs
        && *default_scroll_shift == right.default_scroll_shift
        && *orthogonal_flow_baselines == right.orthogonal_flow_baselines
}

fn collect_nodes_of_fragment_subtrees(links: &[fragment_tree::FragmentLink], nodes: &mut HashSet<u32>) {
    for link in links {
        nodes.insert(link.fragment.node.slot_index());
        collect_nodes_of_fragment_subtrees(&link.fragment.children, nodes);
    }
}

/// Compares the descendant fragments a run produced with those a fresh layout of its root produced. Where the run
/// reused a committed subtree, the committed fragments stand for the ones it did not lay out again.
struct FreshDescendantComparison<'comparison, 'pass> {
    callbacks: &'comparison LayoutPass<'pass>,
    reused_subtree_roots: &'comparison HashSet<u32>,
    nodes_laid_out_afresh: HashSet<u32>,
}

impl FreshDescendantComparison<'_, '_> {
    fn compare_children(
        &self,
        parent: Node,
        expected: &[fragment_tree::FragmentLink],
        fresh: &[fragment_tree::FragmentLink],
        expected_is_committed: bool,
    ) -> Result<(), String> {
        if expected.is_empty() && expected_is_committed {
            return self.compare_with_committed_fragments_of_descendants(parent, fresh);
        }
        // NB: Below a subtree the run reused, the run holds the committed fragments, whose children a commit that
        //     reused them left out.
        if !expected_is_committed && self.reused_subtree_roots.contains(&parent.slot_index()) {
            return self.compare_with_committed_descendants(parent, fresh);
        }
        if expected.len() != fresh.len() {
            return Err(format!(
                "{parent:?} has children {:?} where the fresh layout has {:?}",
                nodes_of(expected),
                nodes_of(fresh)
            ));
        }
        for (index, fresh_link) in fresh.iter().enumerate() {
            let node = fresh_link.fragment.node;
            let expected_link = if expected[index].fragment.node == node {
                &expected[index]
            } else {
                let occurrence = fresh[..index].iter().filter(|link| link.fragment.node == node).count();
                expected
                    .iter()
                    .filter(|link| link.fragment.node == node)
                    .nth(occurrence)
                    .ok_or_else(|| {
                        format!(
                            "{parent:?} has children {:?} where the fresh layout has {:?}",
                            nodes_of(expected),
                            nodes_of(fresh)
                        )
                    })?
            };
            self.compare_links(expected_link, fresh_link, expected_is_committed)?;
        }
        Ok(())
    }

    fn compare_with_committed_descendants(
        &self,
        parent: Node,
        fresh: &[fragment_tree::FragmentLink],
    ) -> Result<(), String> {
        let committed_children = self
            .callbacks
            .arena()
            .with_current_committed_fragment(parent, |fragment| fragment.children.clone())
            .unwrap_or_default();
        if !committed_children.is_empty() {
            return self.compare_children(parent, &committed_children, fresh, true);
        }
        self.compare_with_committed_fragments_of_descendants(parent, fresh)
    }

    fn compare_with_committed_fragments_of_descendants(
        &self,
        parent: Node,
        fresh: &[fragment_tree::FragmentLink],
    ) -> Result<(), String> {
        self.ensure_every_committed_descendant_is_laid_out_afresh(parent)?;
        for (index, fresh_link) in fresh.iter().enumerate() {
            let node = fresh_link.fragment.node;
            let fragment_index = fresh[..index].iter().filter(|link| link.fragment.node == node).count();
            let Some(committed_link) = self.committed_fragment_link(node, fragment_index) else {
                return Err(format!(
                    "{node:?}, a child of the reused subtree of {parent:?}, has no committed fragment {fragment_index}"
                ));
            };
            self.compare_links(&committed_link, fresh_link, true)?;
            let is_last_fresh_fragment = !fresh[index + 1..].iter().any(|link| link.fragment.node == node);
            if is_last_fresh_fragment && self.committed_fragment_link(node, fragment_index + 1).is_some() {
                return Err(format!(
                    "{node:?}, a child of the reused subtree of {parent:?}, has more committed fragments than the {} of \
                     the fresh layout",
                    fragment_index + 1
                ));
            }
        }
        Ok(())
    }

    fn ensure_every_committed_descendant_is_laid_out_afresh(&self, subtree_root: Node) -> Result<(), String> {
        let mut committed_descendant_missing_from_fresh_layout = None;
        self.callbacks
            .arena()
            .for_each_node_in_layout_subtree_in_pre_order(subtree_root, |node| {
                if node != subtree_root
                    && committed_descendant_missing_from_fresh_layout.is_none()
                    && self.callbacks.has_committed_fragment_link(node)
                    && !self.nodes_laid_out_afresh.contains(&node.slot_index())
                {
                    committed_descendant_missing_from_fresh_layout = Some(node);
                }
            });
        match committed_descendant_missing_from_fresh_layout {
            None => Ok(()),
            Some(node) => Err(format!(
                "{node:?}, a descendant of the reused subtree of {subtree_root:?}, has a committed fragment where the \
                 fresh layout has none"
            )),
        }
    }

    fn committed_fragment_link(&self, node: Node, fragment_index: usize) -> Option<fragment_tree::FragmentLink> {
        if fragment_index == 0 {
            return self.callbacks.committed_fragment_link(node);
        }
        let arena = self.callbacks.arena();
        if !arena.paintable_row_is_populated(node) {
            return None;
        }
        arena
            .committed_side_data(node)
            .box_fragments()
            .get(fragment_index - 1)
            .map(|later_fragment| later_fragment.link.clone())
    }

    fn compare_links(
        &self,
        expected: &fragment_tree::FragmentLink,
        fresh: &fragment_tree::FragmentLink,
        expected_is_committed: bool,
    ) -> Result<(), String> {
        let node = fresh.fragment.node;
        if !expected.has_same_placement(fresh) {
            return Err(format!(
                "{node:?} is placed at {} where the fresh layout places it at {}",
                describe_placement(expected),
                describe_placement(fresh)
            ));
        }
        let (expected, fresh) = (&expected.fragment, &fresh.fragment);
        if !expected.has_same_box_properties_comparing_payloads_by_value(fresh) {
            return Err(format!("{node:?} has box properties unlike the fresh layout"));
        }
        if expected.line_data.as_deref() != fresh.line_data.as_deref() {
            return Err(format!("{node:?} has line data unlike the fresh layout"));
        }
        self.compare_children(node, &expected.children, &fresh.children, expected_is_committed)
    }
}

fn nodes_of(links: &[fragment_tree::FragmentLink]) -> Vec<Node> {
    links.iter().map(|link| link.fragment.node).collect()
}

fn describe_placement(link: &fragment_tree::FragmentLink) -> String {
    format!(
        "offset ({:?}, {:?}) insets ({:?} {:?} {:?} {:?}) line box {:?} in {:?}, abspos inputs {}",
        link.committed_offset.x,
        link.committed_offset.y,
        link.inset_top,
        link.inset_right,
        link.inset_bottom,
        link.inset_left,
        link.containing_line_box_index,
        link.containing_block,
        if link.abspos_layout_inputs.is_some() {
            "present"
        } else {
            "absent"
        }
    )
}
