/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(super) fn compute_inline_sizes(
    callbacks: LayoutPass<'_>,
    node: Node,
    root: std::rc::Rc<UsedValues>,
    constraints: ContainingBlockConstraints,
    block_size: AvailableSize,
) -> Option<IntrinsicInlineSizeMeasurement> {
    let facts = NodeFacts::new(&callbacks, node);
    let style = StyleValues::for_node(&callbacks, node);
    // NB: These contexts can couple inline size to block layout or have special content sizing rules.
    if !facts.children_are_inline()
        || formatting_context::independent_formatting_context_type(node, &callbacks)
            != formatting_context::FfiFormattingContextType::Block
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
    RunRecords::with_root(callbacks.arena(), node, root, |records| {
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
        let parent = block_formatting_context::BlockFormattingContext::new(&run);
        let mut context = inline_formatting_context::InlineFormattingContext::new_with_rust_parent(
            &run,
            node,
            LayoutMode::IntrinsicSizing,
            input,
            callbacks,
            &parent,
        );
        let iterator = inline_level_iterator::InlineLevelIterator::for_intrinsic_inline_size(&mut context)?;
        let maximum = context.max_content_inline_size_from_items(iterator.items())?;
        let minimum = context.min_content_inline_size_from_max_content_items(iterator.items());
        Some(IntrinsicInlineSizeMeasurement {
            automatic_content_inline_size: clamp_to_max_dimension_value(maximum),
            min_content_inline_size_from_max_content_layout: minimum.map(clamp_to_max_dimension_value),
            layout: None,
            depends_on_percentage_block_size: sizing.resolve_percentage_block_size_dependency(node),
            depends_on_percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(node),
        })
    })
}
