/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The formatting context of a replaced box with children, whose shadow content lays out in an anonymous wrapper.
#[derive(Default)]
pub(crate) struct ReplacedWithChildrenFormattingContext;

impl<'pass> formatting_context::FormattingContextAlgorithm<'pass> for ReplacedWithChildrenFormattingContext {
    fn lay_out(
        &mut self,
        run: &formatting_context::FormattingContextRun<'pass>,
        input: formatting_context::AlgorithmInput<'_>,
    ) -> formatting_context::AlgorithmOutput {
        let mut root = input.root_cells;
        let result = layout_replaced_with_children(run, input.body_input, &mut root);
        formatting_context::AlgorithmOutput::of_root(result, root)
    }
}

fn layout_replaced_with_children(
    run: &formatting_context::FormattingContextRun,
    layout_input: geometry::LayoutInput,
    root: &mut used_values::RootInputCells,
) -> formatting_context::ChildLayoutResult {
    // The parent FC has already resolved this replaced box's used size (natural size, explicit size, or the default
    // object size), so both dimensions are definite for its children — shadow content sized to fill
    // (e.g. width/height: 100%) resolves against them.
    root.has_definite_inline_size = true;
    root.has_definite_block_size = true;
    let content_inline_size = root.content_inline_size;
    let root_content_block_size = root.content_block_size;

    let child_available_space = geometry::AvailableSpace {
        inline_size: geometry::AvailableSize::definite(content_inline_size),
        block_size: geometry::AvailableSize::definite(root_content_block_size),
    };

    // The TreeBuilder wraps shadow DOM children in an anonymous BlockContainer.
    // Delegate layout to a BFC for that wrapper.
    let Some(wrapper) = run
        .pass
        .children(run.box_)
        .find(|&child| node_facts::NodeFacts::new(&run.pass, child).is_block_container())
    else {
        return formatting_context::ChildLayoutResult::default();
    };

    let wrapper_constraints =
        run.sizing()
            .constraints_for_child_context(run.box_, &*root, layout_input.containing_block_constraints);
    let mut wrapper_cells = used_values::cells_before_sizing(&run.pass, wrapper, wrapper_constraints);
    wrapper_cells.set_content_inline_size(content_inline_size);

    let plan = formatting_context::ChildRunPlan {
        fc_type: formatting_context::FormattingContextType::Block,
        root_containing_block: run.pass.in_flow_containing_block(wrapper),
        input: geometry::LayoutInput {
            available_space: child_available_space,
            containing_block_constraints: wrapper_constraints,
            sizing: geometry::RootSizingDirectives {
                content_block_size_after_run: geometry::ContentBlockSizeAfterRun::Automatic,
                ..geometry::RootSizingDirectives::default()
            },
            participation: geometry::ParticipationInParentFormattingContext::Item,
        },
    };
    let wrapper_outcome = formatting_context::run_formatting_context(
        run.pass,
        formatting_context::RunRequest::of_child(run, wrapper, plan, wrapper_cells),
        formatting_context::RunSideInputs::default(),
    );
    let wrapper_result = *wrapper_outcome.result().expect("a run creates its root's result");
    wrapper_outcome.report_percentage_block_size_dependency_of(run, wrapper);
    formatting_context::place_child(
        run,
        wrapper,
        wrapper_outcome.placed_child(),
        used_values::FfiCssPixelPoint::default(),
        None,
        wrapper_outcome.into_fragment_arrival(run),
    );

    formatting_context::ChildLayoutResult {
        automatic_content_block_size: wrapper_result.automatic_content_block_size,
        baselines: formatting_context::DerivedBaselines::default(),
        ..formatting_context::ChildLayoutResult::default()
    }
}
