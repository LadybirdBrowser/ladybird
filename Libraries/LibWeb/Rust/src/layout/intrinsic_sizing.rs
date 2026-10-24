/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(super) struct ContentSizingScope {
    node: Node,
    root: UsedValues,
}

impl ContentSizingScope {
    pub(super) fn new(
        callbacks: &LayoutPass<'_>,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
        inline_size_constraint: SizeConstraint,
    ) -> Self {
        let root = used_values::create_used_values(callbacks, node, constraints);
        // NB: A parent layout can assign a definite block size that is not present in computed style,
        //     such as the stretched cross size of a flex item. Preserve it so descendant percentages
        //     resolve against the same size during intrinsic measurement.
        if let Some(block_size) = block_size {
            root.set_content_block_size(block_size);
            root.has_definite_block_size.set(true);
        }
        root.inline_size_constraint.set(inline_size_constraint);
        root.has_definite_inline_size.set(false);
        Self { node, root }
    }

    pub(super) fn root(&self) -> &UsedValues {
        &self.root
    }

    pub(super) fn available_block_size(&self) -> AvailableSize {
        if self.root.has_definite_block_size() {
            AvailableSize::definite(self.root.content_block_size.get())
        } else {
            AvailableSize::Indefinite
        }
    }

    fn enter<R>(&self, callbacks: LayoutPass<'_>, size_content: impl FnOnce(&FormattingContextRun<'_>) -> R) -> R {
        RunRecords::with_content_sizing_root(
            callbacks,
            self.node,
            callbacks.in_flow_containing_block(self.node),
            &self.root,
            |records, callbacks| {
                size_content(&FormattingContextRun {
                    purpose: formatting_context::LayoutPurpose::Measurement,
                    records,
                    box_: self.node,
                    layout_mode: LayoutMode::IntrinsicSizing,
                    callbacks,
                    should_collect_devtools_layout_data: false,
                    treat_block_axis_percentage_insets_as_auto_beyond_root: false,
                    fragments: None,
                    previous_line_data: None,
                    fragmentainer: None,
                })
            },
        )
    }
}

pub(super) fn compute_inline_sizes(
    callbacks: LayoutPass<'_>,
    scope: &ContentSizingScope,
    constraints: ContainingBlockConstraints,
    depends_on_block_size: bool,
) -> Option<IntrinsicInlineSizeMeasurement> {
    let node = scope.node;
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
        || facts.node_has_inline_size_containment()
        || !inline_formatting_context::inline_content_is_measurable_from_items(
            &facts,
            style,
            constraints.inline_basis(),
        )
    {
        return None;
    }
    scope.enter(callbacks, |run| {
        let sizing = run.sizing();
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: scope.available_block_size(),
            },
            sizing.constraints_for_child_context(node, constraints),
            ParticipationInParentFormattingContext::BlockLevel,
        );
        let measurement = compute_from_atomic_children(run, input, depends_on_block_size)
            .or_else(|| compute_from_items(run, input, depends_on_block_size))?;
        if !facts.lays_out_in_columns() {
            return Some(measurement);
        }
        Some(IntrinsicInlineSizeMeasurement {
            automatic_content_inline_size: fragmentation::multicol_intrinsic_inline_size(
                style,
                measurement.automatic_content_inline_size,
                false,
            ),
            min_content_inline_size_from_max_content_layout: measurement
                .min_content_inline_size_from_max_content_layout
                .map(|minimum| fragmentation::multicol_intrinsic_inline_size(style, minimum, true)),
            ..measurement
        })
    })
}

fn compute_from_items(
    run: &FormattingContextRun<'_>,
    input: LayoutInput,
    depends_on_block_size: bool,
) -> Option<IntrinsicInlineSizeMeasurement> {
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
        depends_on_percentage_block_size: sizing.resolve_percentage_block_size_dependency(run.box_),
        depends_on_percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(run.box_),
        depends_on_block_size,
    })
}

fn compute_from_atomic_children(
    run: &FormattingContextRun<'_>,
    input: LayoutInput,
    depends_on_block_size: bool,
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
    for child in callbacks.children(run.box_) {
        let facts = NodeFacts::new(&callbacks, child);
        if facts.data().kind.get() != NodeKind::BlockContainer
            || !facts.is_atomic_inline()
            || facts.is_floating_or_absolutely_positioned()
            || facts.is_fragmented_inline()
        {
            return None;
        }
    }
    let wraps = containing_style.text_wrap_mode() == text_wrap_mode::WRAP;
    let mut max_content_lines = inline_formatting_context::LinesWithoutLineBoxes::new(false);
    let mut min_content_lines = inline_formatting_context::LinesWithoutLineBoxes::new(true);
    for child in callbacks.children(run.box_) {
        let contributions = sizing.outer_inline_contributions(
            child,
            input.available_space.block_size,
            input.containing_block_constraints,
        );
        max_content_lines.append_atomic_inline(contributions.outer_max_content_contribution(), wraps);
        min_content_lines.append_atomic_inline(contributions.outer_min_content_contribution(), wraps);
    }
    Some(IntrinsicInlineSizeMeasurement {
        automatic_content_inline_size: clamp_to_max_dimension_value(max_content_lines.finish_measurement()),
        min_content_inline_size_from_max_content_layout: Some(clamp_to_max_dimension_value(
            min_content_lines.finish_measurement(),
        )),
        depends_on_percentage_block_size: sizing.resolve_percentage_block_size_dependency(run.box_),
        depends_on_percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(run.box_),
        depends_on_block_size,
    })
}
