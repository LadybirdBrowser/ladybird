/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/Editing/MutationTrackedRange.h>

namespace Web::Editing {

MutationTrackedRange::MutationTrackedRange(GC::Ref<DOM::Range> range)
    : m_range(range)
{
    ranges().set(range.ptr());
}

MutationTrackedRange::~MutationTrackedRange()
{
    ranges().remove(m_range.ptr());
}

HashTable<DOM::Range*>& MutationTrackedRange::ranges()
{
    static NeverDestroyed<HashTable<DOM::Range*>> ranges;
    return ranges.get();
}

}
