/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

fn run(frame: &mut FcFrame, layout_input: LayoutInput) {
    // The parent FC has already resolved this replaced box's used size (natural size, explicit size, or the default
    // object size), so both dimensions are definite for its children — shadow content sized to fill
    // (e.g. width/height: 100%) resolves against them.
    let (content_inline_size, root_content_block_size) = {
        let root_state = frame.state.used_values(&frame.callbacks, frame.box_);
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
    let mut wrapper = frame.callbacks.first_child(frame.box_);
    while !wrapper.is_invalid() {
        if frame.state.node_facts(&frame.callbacks, wrapper).is_block_container() {
            break;
        }
        wrapper = frame.callbacks.next_sibling(wrapper);
    }
    if wrapper.is_invalid() {
        return;
    }

    let wrapper_constraints = SizingContext::new(frame.state, frame.callbacks)
        .constraints_for_child_context(frame.box_, layout_input.containing_block_constraints);
    let wrapper_state = frame
        .state
        .create_used_values(&frame.callbacks, wrapper, wrapper_constraints);
    wrapper_state.set_content_inline_size(content_inline_size);

    let mut bfc = crate::layout::create_formatting_context(
        frame.state,
        wrapper,
        crate::layout::FcParents::default(),
        FfiFormattingContextType::Block,
        frame.layout_mode,
        frame.should_collect_devtools_layout_data,
        frame.callbacks,
    );
    crate::layout::run_formatting_context(
        &mut bfc,
        LayoutInput {
            available_space: child_available_space,
            containing_block_constraints: wrapper_constraints,
            content_box_position_in_bfc_root: None,
            table_grid_min_border_box_block_size: None,
        },
        None,
        None,
    );

    frame.automatic_content_inline_size = content_inline_size;
    frame.automatic_content_block_size = bfc.automatic_content_block_size;

    wrapper_state.set_content_block_size(frame.automatic_content_block_size);

    crate::layout::place_child(frame.state, &frame.callbacks, wrapper, FfiCssPixelPoint::default());

    crate::layout::parent_did_dimension(&mut bfc);
}
