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
        || facts.is_scroll_container()
        || style.writing_mode() != writing_mode::HORIZONTAL_TB
        || style.text_indent().to_px(constraints.inline_basis()) != CssPixels::default()
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
        let maximum = max_content_inline_size(&context, iterator.items())?;
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

// https://drafts.csswg.org/css-sizing-3/#max-content-inline-size
// Usually the narrowest inline size it could take while fitting around its contents
// if none of the soft wrap opportunities within the box were taken.
fn max_content_inline_size(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    items: &[inline_level_iterator::Item],
) -> Option<CssPixels> {
    let mut maximum = CssPixels::default();
    let mut current = CssPixels::default();
    let mut trailing_whitespace = CssPixels::default();
    let mut leading_margin = CssPixels::default();
    let mut leading_border = CssPixels::default();
    let mut leading_padding = CssPixels::default();
    let mut line_is_empty_or_ends_in_whitespace = true;
    for item in items {
        use inline_level_iterator::ItemType;
        if matches!(item.type_, ItemType::FloatingElement | ItemType::BlockLevelBox) {
            return None;
        }
        if item.type_ == ItemType::AbsolutelyPositionedElement {
            leading_margin = CssPixels::default();
            leading_border = CssPixels::default();
            leading_padding = CssPixels::default();
            continue;
        }
        if item.type_ == ItemType::ForcedBreak {
            maximum = maximum.max(current - trailing_whitespace);
            current = CssPixels::default();
            leading_margin = CssPixels::default();
            leading_border = CssPixels::default();
            leading_padding = CssPixels::default();
            trailing_whitespace = CssPixels::default();
            line_is_empty_or_ends_in_whitespace = true;
            continue;
        }
        if item.is_collapsible_whitespace && line_is_empty_or_ends_in_whitespace {
            leading_margin += item.margin_start;
            leading_border += item.border_start;
            leading_padding += item.padding_start;
            continue;
        }
        current += line_box::inline_advance(
            item.margin_start + leading_margin,
            (item.border_start + leading_border) + (item.padding_start + leading_padding),
            item.inline_size,
            item.padding_end + item.border_end,
            item.margin_end,
        );
        leading_margin = CssPixels::default();
        leading_border = CssPixels::default();
        leading_padding = CssPixels::default();
        if item.type_ == ItemType::Text {
            let style = context.style(context.style_source(item.node));
            if style.writing_mode() != writing_mode::HORIZONTAL_TB {
                return None;
            }
            let collapses = matches!(
                style.white_space_collapse(),
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            );
            let all_whitespace = item.is_ascii_whitespace(context);
            if collapses && all_whitespace {
                trailing_whitespace += item.inline_size;
            } else if collapses {
                trailing_whitespace = item.trailing_whitespace.inline_size;
            } else {
                trailing_whitespace = CssPixels::default();
            }
            line_is_empty_or_ends_in_whitespace = context.callbacks.text_content(item.node).text
                [item.offset_in_node..item.offset_in_node + item.length_in_node]
                .last()
                .is_some_and(|unit| line_box_fragment::is_ascii_space(*unit));
        } else {
            trailing_whitespace = CssPixels::default();
            line_is_empty_or_ends_in_whitespace = false;
        }
    }
    Some(maximum.max(current - trailing_whitespace))
}
