/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

fn layout_replaced_with_children(run: &FormattingContextRun, layout_input: LayoutInput) -> ChildLayoutResult {
    // The parent FC has already resolved this replaced box's used size (natural size, explicit size, or the default
    // object size), so both dimensions are definite for its children — shadow content sized to fill
    // (e.g. width/height: 100%) resolves against them.
    let (content_inline_size, root_content_block_size) = {
        let root_state = run.state.used_values(&run.callbacks, run.box_);
        root_state.has_definite_inline_size.set(true);
        root_state.has_definite_block_size.set(true);
        (
            root_state.content_inline_size.get(),
            root_state.content_block_size.get(),
        )
    };

    let child_available_space = AvailableSpace {
        inline_size: AvailableSize::definite(content_inline_size),
        block_size: AvailableSize::definite(root_content_block_size),
    };

    // The TreeBuilder wraps shadow DOM children in an anonymous BlockContainer.
    // Delegate layout to a BFC for that wrapper.
    let mut wrapper = run.callbacks.first_child(run.box_);
    while !wrapper.is_invalid() {
        if run.state.node_facts(&run.callbacks, wrapper).is_block_container() {
            break;
        }
        wrapper = run.callbacks.next_sibling(wrapper);
    }
    if wrapper.is_invalid() {
        return ChildLayoutResult::default();
    }

    let wrapper_constraints = SizingContext::new(run.state, run.callbacks)
        .constraints_for_child_context(run.box_, layout_input.containing_block_constraints);
    let wrapper_state = run
        .state
        .create_used_values(&run.callbacks, wrapper, wrapper_constraints);
    wrapper_state.set_content_inline_size(content_inline_size);

    let wrapper_layout = crate::layout::run_formatting_context(
        run.state,
        wrapper,
        None,
        FfiFormattingContextType::Block,
        run.layout_mode,
        run.should_collect_devtools_layout_data,
        run.callbacks,
        LayoutInput {
            available_space: child_available_space,
            containing_block_constraints: wrapper_constraints,
            content_box_position_in_bfc_root: None,
            sizing: RootSizingDirectives {
                adopt_automatic_content_block_size: true,
                ..RootSizingDirectives::default()
            },
            participation: ParticipationInParentFormattingContext::Item,
        },
        None,
    );

    crate::layout::place_child(run.state, &run.callbacks, wrapper, FfiCssPixelPoint::default());

    ChildLayoutResult {
        automatic_content_inline_size: content_inline_size,
        automatic_content_block_size: wrapper_layout.automatic_content_block_size,
    }
}
