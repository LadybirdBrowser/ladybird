/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(super) fn compute_inline_sizes(
    callbacks: LayoutPass<'_>,
    node: Node,
    node_containing_block: Node,
    root: &UsedValues,
    constraints: ContainingBlockConstraints,
    block_size: AvailableSize,
) -> Option<IntrinsicInlineSizeMeasurement> {
    let facts = NodeFacts::new(&callbacks, node);
    let style = StyleValues::for_node(&callbacks, node);
    // NB: These contexts can couple inline size to block layout or have special content sizing rules.
    if !facts.children_are_inline()
        || formatting_context::independent_formatting_context_type(node, &callbacks)
            != formatting_context::FormattingContextType::Block
        || facts.is_table_wrapper()
        || facts.is_fieldset_box()
        || facts.uses_button_layout()
        || facts.node_has_size_containment()
        || !inline_formatting_context::inline_content_is_measurable_from_items(
            &facts,
            style,
            constraints.inline_basis(),
        )
    {
        return None;
    }
    RunRecords::with_root(callbacks.arena(), node, node_containing_block, root, |records| {
        let run = FormattingContextRun {
            purpose: formatting_context::LayoutPurpose::Measurement,
            records,
            box_: node,
            layout_mode: LayoutMode::IntrinsicSizing,
            callbacks,
            should_collect_devtools_layout_data: false,
            treat_block_axis_percentage_insets_as_auto_beyond_root: false,
            fragments: None,
            previous_line_data: None,
        };
        let sizing = run.sizing();
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size,
            },
            sizing.constraints_for_child_context(node, constraints),
            ParticipationInParentFormattingContext::BlockLevel,
        );
        if let Some(measurement) = compute_from_atomic_children(&run, input) {
            if fc_run_cache::fc_run_cache_mode_from_environment() == fc_run_cache::FcRunCacheMode::Shadow {
                assert_eq!(Some(measurement), compute_from_items(&run, input));
            }
            return Some(measurement);
        }
        compute_from_items(&run, input)
    })
}

fn compute_from_items(run: &FormattingContextRun<'_>, input: LayoutInput) -> Option<IntrinsicInlineSizeMeasurement> {
    let sizing = run.sizing();
    let parent = block_formatting_context::BlockFormattingContext::new(run);
    let mut context = inline_formatting_context::InlineFormattingContext::new_with_rust_parent(
        run,
        run.box_,
        LayoutMode::IntrinsicSizing,
        input,
        run.callbacks,
        &parent,
    );
    let iterator = inline_level_iterator::InlineLevelIterator::for_intrinsic_inline_size(&mut context)?;
    let maximum = context
        .intrinsic_inline_size_from_items(iterator.items(), inline_formatting_context::ItemMeasurement::MaxContent)?;
    let minimum = context.min_content_inline_size_from_max_content_items(iterator.items());
    iterator.stash_for_reuse(&context);
    Some(IntrinsicInlineSizeMeasurement {
        automatic_content_inline_size: clamp_to_max_dimension_value(maximum),
        min_content_inline_size_from_max_content_layout: minimum.map(clamp_to_max_dimension_value),
        layout: None,
        depends_on_percentage_block_size: sizing.resolve_percentage_block_size_dependency(run.box_),
        depends_on_percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(run.box_),
    })
}

fn compute_from_atomic_children(
    run: &FormattingContextRun<'_>,
    input: LayoutInput,
) -> Option<IntrinsicInlineSizeMeasurement> {
    let callbacks = run.callbacks;
    let containing_style = StyleValues::for_node(&callbacks, run.box_);
    if !inline_formatting_context::inline_content_is_measurable_from_items(
        &NodeFacts::new(&callbacks, run.box_),
        containing_style,
        input.containing_block_constraints.inline_basis(),
    ) {
        return None;
    }
    let sizing = run.sizing();
    let mut child = callbacks.first_child(run.box_);
    while !child.is_invalid() {
        let facts = NodeFacts::new(&callbacks, child);
        if facts.data().kind.get() != NodeKind::BlockContainer
            || !facts.is_atomic_inline()
            || facts.is_floating_or_absolutely_positioned()
            || facts.is_fragmented_inline()
            || facts.node_has_size_containment()
            || StyleValues::for_node(&callbacks, child).box_sizing() != box_sizing::CONTENT_BOX
            || !sizing.atomic_inline_size_follows_from_style(child)
        {
            return None;
        }
        child = callbacks.next_sibling(child);
    }
    let wraps = containing_style.text_wrap_mode() == text_wrap_mode::WRAP;
    let mut max_content_lines = inline_formatting_context::LinesWithoutLineBoxes::new(false);
    let mut min_content_lines = Some(inline_formatting_context::LinesWithoutLineBoxes::new(true));
    child = callbacks.first_child(run.box_);
    while !child.is_invalid() {
        let contribution =
            sizing.atomic_inline_contribution(child, input.available_space, input.containing_block_constraints);
        max_content_lines.append_atomic_inline(contribution.inline_advance(contribution.content_inline_size), wraps);
        min_content_lines = min_content_lines
            .zip(contribution.min_content_inline_size)
            .map(|(mut lines, content)| {
                lines.append_atomic_inline(contribution.inline_advance(content), wraps);
                lines
            });
        child = callbacks.next_sibling(child);
    }
    Some(IntrinsicInlineSizeMeasurement {
        automatic_content_inline_size: clamp_to_max_dimension_value(max_content_lines.finish_measurement()),
        min_content_inline_size_from_max_content_layout: min_content_lines
            .map(|lines| clamp_to_max_dimension_value(lines.finish_measurement())),
        layout: None,
        depends_on_percentage_block_size: sizing.resolve_percentage_block_size_dependency(run.box_),
        depends_on_percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(run.box_),
    })
}
