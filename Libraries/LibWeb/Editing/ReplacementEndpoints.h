/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWeb/Editing/MutationTrackedRange.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

// Own the semantic boundaries of inserted content independently. A DOM Range is unsuitable while completing a rich
// replacement because paragraph moves may temporarily reorder its endpoints. Blink and WebKit use independent
// Positions for these boundaries, while Gecko uses tracked DOM points. Two collapsed mutation-tracked ranges provide
// the same invariant here: each endpoint follows its content without being constrained by the other endpoint.
class ReplacementEndpoints {
    AK_MAKE_NONCOPYABLE(ReplacementEndpoints);
    AK_MAKE_NONMOVABLE(ReplacementEndpoints);

public:
    ReplacementEndpoints(DOM::BoundaryPoint start, DOM::BoundaryPoint end);

    DOM::BoundaryPoint start() const;
    DOM::BoundaryPoint end() const;

    void set_start(DOM::BoundaryPoint);
    void set_end(DOM::BoundaryPoint);

    void prepare_for_merging_left_text_into_right(DOM::Text& left, DOM::Text& right, u32 left_length);
    void prepare_for_merging_right_text_into_left(DOM::Text& left, DOM::Text& right, u32 left_length);

private:
    void relocate_for_merging_left_text_into_right(MutationTrackedRange&, DOM::Text& left, DOM::Text& right, u32 left_length);
    void relocate_for_merging_right_text_into_left(MutationTrackedRange&, DOM::Text& left, DOM::Text& right, u32 left_length);

    OwnPtr<MutationTrackedRange> m_start;
    OwnPtr<MutationTrackedRange> m_end;
};

}
