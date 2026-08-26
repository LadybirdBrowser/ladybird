/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(super) fn layout_replaced_with_children(
    run: &formatting_context::FormattingContextRun,
    layout_input: geometry::LayoutInput,
) -> formatting_context::ChildLayoutResult {
    // The parent FC has already resolved this replaced box's used size (natural size, explicit size, or the default
    // object size), so both dimensions are definite for its children — shadow content sized to fill
    // (e.g. width/height: 100%) resolves against them.
    let (content_inline_size, root_content_block_size) = {
        let root_state = run.records.used_values(run.box_);
        root_state.has_definite_inline_size.set(true);
        root_state.has_definite_block_size.set(true);
        (
            root_state.content_inline_size.get(),
            root_state.content_block_size.get(),
        )
    };

    let child_available_space = geometry::AvailableSpace {
        inline_size: geometry::AvailableSize::definite(content_inline_size),
        block_size: geometry::AvailableSize::definite(root_content_block_size),
    };

    // The TreeBuilder wraps shadow DOM children in an anonymous BlockContainer.
    // Delegate layout to a BFC for that wrapper.
    let mut wrapper = run.callbacks.first_child(run.box_);
    while !wrapper.is_invalid() {
        if node_facts::NodeFacts::new(&run.callbacks, wrapper).is_block_container() {
            break;
        }
        wrapper = run.callbacks.next_sibling(wrapper);
    }
    if wrapper.is_invalid() {
        return formatting_context::ChildLayoutResult::default();
    }

    let wrapper_constraints = run
        .sizing()
        .constraints_for_child_context(run.box_, layout_input.containing_block_constraints);
    let wrapper_state = run
        .records
        .create_used_values(&run.callbacks, wrapper, wrapper_constraints);
    wrapper_state.set_content_inline_size(content_inline_size);

    let wrapper_result = formatting_context::run_formatting_context(
        run.purpose,
        run.fragments.as_deref(),
        &wrapper_state,
        wrapper,
        None,
        formatting_context::FfiFormattingContextType::Block,
        run.layout_mode,
        run.should_collect_devtools_layout_data,
        run.callbacks,
        geometry::LayoutInput {
            available_space: child_available_space,
            containing_block_constraints: wrapper_constraints,
            content_box_position_in_bfc_root: None,
            sizing: geometry::RootSizingDirectives {
                adopt_automatic_content_block_size: true,
                ..geometry::RootSizingDirectives::default()
            },
            participation: geometry::ParticipationInParentFormattingContext::Item,
        },
        None,
    );
    formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
        &run.records,
        &run.callbacks,
        wrapper,
        wrapper_result.depends_on_percentage_block_size,
    );
    formatting_context::place_child(run, wrapper, used_values::FfiCssPixelPoint::default(), None);

    formatting_context::ChildLayoutResult {
        automatic_content_inline_size: content_inline_size,
        automatic_content_block_size: wrapper_result.automatic_content_block_size,
        baselines: formatting_context::DerivedBaselines::default(),
        ..formatting_context::ChildLayoutResult::default()
    }
}
