/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/LegacyFontStyle.h>
#include <LibWeb/Editing/MutationTrackedRange.h>
#include <LibWeb/Editing/ParagraphTransfer.h>

namespace Web::Editing {

ParagraphTransferResult transfer_paragraph_contents(DOM::Node& source_paragraph, DOM::BoundaryPoint destination, ParagraphTransferOptions options)
{
    VERIFY(source_paragraph.parent());

    // Style preparation can replace one legacy wrapper with several inline runs. Track the semantic destination
    // before that topology change so the transfer remains on the same side of all replacement nodes.
    MutationTrackedRange tracked_destination { DOM::Range::create(destination.node, destination.offset, destination.node, destination.offset) };

    if (options.source_style == ParagraphTransferStyle::MatchDestination) {
        // INTEROP: Blink's nested replacement for a paragraph move applies the destination paragraph's style when
        //          moving a single partial presentational run into a list item. Model that as an explicit transfer
        //          policy instead of coupling paragraph movement to legacy font markup.
        remove_single_run_legacy_font_style(source_paragraph);
    } else if (options.source_style == ParagraphTransferStyle::PreserveVisualAppearance) {
        materialize_single_run_legacy_font_style(source_paragraph);
    }
    if (options.destination_style == ParagraphTransferStyle::PreserveVisualAppearance) {
        auto destination_paragraph = block_node_of_node(*destination.node);
        if (destination_paragraph)
            materialize_single_run_legacy_font_style(*destination_paragraph);
    }

    if (!source_paragraph.has_children()) {
        remove_node(source_paragraph);
        auto insertion_point = tracked_destination.range().start();
        return ParagraphTransferResult {
            .first_node = insertion_point.node,
            .last_node = insertion_point.node,
            .start = insertion_point,
            .end = insertion_point,
            .destination_text_seam = insertion_point,
        };
    }

    GC::Root<DOM::Node> first_moved_node { *source_paragraph.first_child() };
    GC::Root<DOM::Node> last_moved_node { *source_paragraph.last_child() };
    auto insertion_point = tracked_destination.range().start();
    if (source_paragraph.parent() == insertion_point.node.ptr() && source_paragraph.index() == insertion_point.offset) {
        unwrap_node_preserving_ranges(source_paragraph);
    } else {
        auto insertion_offset = insertion_point.offset;
        while (source_paragraph.first_child())
            move_node_preserving_ranges(*source_paragraph.first_child(), *insertion_point.node, insertion_offset++);
        remove_node(source_paragraph);
    }

    auto start = tracked_destination.range().start();
    auto end = end_boundary_of_node(*last_moved_node);
    Optional<DOM::BoundaryPoint> destination_text_seam;
    if (options.seam == ParagraphTransferSeam::CoalesceDestinationText) {
        auto* destination_text = as_if<DOM::Text>(*first_moved_node);
        auto* inserted_text = destination_text ? as_if<DOM::Text>(destination_text->previous_sibling()) : nullptr;
        if (inserted_text) {
            auto inserted_length = inserted_text->length_in_utf16_code_units();
            MUST(insert_data(*destination_text, 0, inserted_text->data()));
            remove_node(*inserted_text);
            destination_text_seam = DOM::BoundaryPoint { *destination_text, inserted_length };
            start = *destination_text_seam;
            end = end_boundary_of_node(*last_moved_node);
        }
    }

    return ParagraphTransferResult {
        .first_node = first_moved_node,
        .last_node = last_moved_node,
        .start = start,
        .end = end,
        .destination_text_seam = destination_text_seam,
    };
}

}
