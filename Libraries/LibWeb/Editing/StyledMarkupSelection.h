/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

// Capture the rendered boundaries used by annotated clipboard serialization. DOM range endpoints alone cannot
// describe a selected paragraph break because formatting whitespace does not participate in the rendered tree.
// Blink and WebKit likewise derive traversal and interchange boundaries from immutable visible positions.
class StyledMarkupSelection {
public:
    explicit StyledMarkupSelection(DOM::Range&);

    DOM::Range& serialization_range() const { return *m_serialization_range; }
    bool contains_only_interchange_newline() const;
    bool has_leading_interchange_newline() const;
    bool has_trailing_interchange_newline() const;

private:
    VisiblePosition m_visible_start;
    VisiblePosition m_visible_end;
    Optional<VisiblePosition> m_previous_visible_end;
    GC::Root<DOM::Range> m_serialization_range;
};

}
