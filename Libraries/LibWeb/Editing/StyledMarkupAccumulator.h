/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

class StyledMarkupSelection;

struct SerializedText {
    GC::Root<DOM::Text> source;
    GC::Root<DOM::Text> serialized;
};

// Builds the structural clipboard fragment by traversing the selected source subtrees. Keeping this distinct from DOM
// Range cloning gives styled serialization one place to decide which selection-edge ancestors carry structure or
// presentation and to associate emitted content with its source as those policies are applied.
class StyledMarkupAccumulator {
public:
    explicit StyledMarkupAccumulator(StyledMarkupSelection const&);

    GC::Ref<DOM::DocumentFragment> fragment() const { return *m_fragment; }
    Vector<SerializedText> const& serialized_text() const { return m_serialized_text; }
    void remove_partial_block_transport_wrapper();

private:
    enum class SelectionCoverage : u8 {
        Partial,
        Full,
    };

    bool append_selected_node(DOM::Node& source, DOM::Node& destination_parent, SelectionCoverage);
    bool participates_in_selection(DOM::Node&) const;

    GC::Root<DOM::Range> m_range;
    GC::Root<DOM::DocumentFragment> m_fragment;
    Vector<SerializedText> m_serialized_text;
};

}
