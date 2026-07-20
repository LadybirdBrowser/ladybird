/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Selection/CaretNavigation.h>

namespace Web::Selection {

class Selection;

// INTEROP: The web editing specifications do not define caret navigation in enough detail to implement it directly.
// SelectionModifier follows the architecture used by other engines: compute a visual caret destination without
// changing the selection, then apply that result as a single selection mutation. Keeping movement policy here also
// prevents platform key handling and DOM Selection mutation from accumulating layout-specific special cases.
class WEB_API SelectionModifier {
public:
    explicit SelectionModifier(Selection& selection)
        : m_selection(selection)
    {
    }

    void modify(SelectionAlteration, SelectionDirection, SelectionGranularity);
    void select_all();

private:
    GC::Ref<Selection> m_selection;
};

}
