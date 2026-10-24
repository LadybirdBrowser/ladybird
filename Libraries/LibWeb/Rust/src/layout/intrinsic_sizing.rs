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

// NB: A box that establishes an orthogonal flow contributes its block size to the inline sizes measured in the frame
//     of `callbacks`, which only its layout finds.
fn establishes_orthogonal_flow(callbacks: &LayoutPass<'_>, node: Node) -> bool {
    formatting_context::frame_established_by(callbacks, node).is_orthogonal_to(callbacks.frame)
}

pub(super) fn content_sizes_of_inline_content(
    callbacks: LayoutPass<'_>,
    scope: &ContentSizingScope,
    constraints: ContainingBlockConstraints,
    depends_on_block_size: bool,
) -> Option<sizing_context::ContentInlineSizes> {
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
        || style.writing_mode() != writing_mode::HORIZONTAL_TB
        || establishes_orthogonal_flow(&callbacks, node)
    {
        return None;
    }
    scope.enter(callbacks, |run| {
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: scope.available_block_size(),
            },
            run.sizing().constraints_for_child_context(node, constraints),
            ParticipationInParentFormattingContext::BlockLevel,
        );
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
        let sizes = context.content_sizes_from_items(iterator.items(), depends_on_block_size);
        iterator.stash_for_reuse(&context);
        if !facts.lays_out_in_columns() {
            return Some(sizes);
        }
        let max = fragmentation::multicol_intrinsic_inline_size(style, sizes.sizes.max, false);
        Some(sizing_context::ContentInlineSizes {
            sizes: sizing_context::ContentSizes {
                min: fragmentation::multicol_intrinsic_inline_size(style, sizes.sizes.min, true),
                max,
            },
            first_line_max_content: max,
            ..sizes
        })
    })
}
