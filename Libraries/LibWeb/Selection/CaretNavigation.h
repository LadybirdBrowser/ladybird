/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TextAffinity.h>

namespace Web::Selection {

enum class SelectionAlteration : u8 {
    Move,
    Extend,
};

enum class SelectionDirection : u8 {
    Forward,
    Backward,
};

enum class SelectionGranularity : u8 {
    Character,
    Word,
    Line,
    Page,
    LineBoundary,
    // The boundary of the active editing host, which is the effective document for contenteditable navigation.
    DocumentBoundary,
};

// A DOM boundary is not sufficient to identify a visual caret position. At a soft wrap, the same node and offset can
// be painted at the end of one line or the start of the next, so affinity travels with every computed destination.
struct CaretLocation {
    GC::Ref<DOM::Node> node;
    size_t offset { 0 };
    TextAffinity affinity { TextAffinity::Downstream };
};

// Resolves visible caret positions without mutating Selection. Editing operations use this service to share the same
// model of rendered text, atomic inline content, empty lines, and editing-host boundaries as keyboard navigation.
class CaretNavigator {
public:
    explicit CaretNavigator(DOM::Document& document)
        : m_document(document)
    {
    }

    Optional<CaretLocation> move(CaretLocation const&, SelectionAlteration, SelectionDirection, SelectionGranularity, Optional<CSSPixels> preferred_inline_coordinate = {});
    CaretLocation canonical_location_for_editing(CaretLocation const&);
    CaretLocation upstream_equivalent_location(CaretLocation const&);
    Optional<CaretLocation> canonical_location_for_extension(CaretLocation const&, SelectionDirection);
    Optional<CSSPixels> inline_coordinate(CaretLocation const&);

private:
    Optional<CaretLocation> move_to_adjacent_caret_host(CaretLocation const&, SelectionDirection);
    Optional<CaretLocation> move_to_editing_host_boundary(CaretLocation const&, SelectionDirection);
    Optional<CaretLocation> move_by_page(CaretLocation const&, SelectionDirection, CSSPixels inline_coordinate);
    Optional<CaretLocation> move_by_word(CaretLocation const&, SelectionAlteration, SelectionDirection);
    GC::Ptr<DOM::Node> adjacent_caret_host(DOM::Node&, DOM::Node& editing_host, SelectionDirection);
    Optional<CaretLocation> location_before_atomic_inline(DOM::Node&, SelectionAlteration);
    Optional<CaretLocation> location_after_atomic_inline(DOM::Node&);
    Optional<CaretLocation> canonicalize_backward_word_location(CaretLocation const&, SelectionAlteration);
    static DOM::Node& navigation_origin(CaretLocation const&, SelectionDirection);

    GC::Ref<DOM::Document> m_document;
};

}
