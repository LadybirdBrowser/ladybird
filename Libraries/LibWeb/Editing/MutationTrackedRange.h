/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibGC/Root.h>
#include <LibWeb/DOM/Range.h>

namespace Web::Editing {

// A live DOM range whose boundary points also participate in editing's preserving-ranges move operation. Script
// ranges retain normal DOM mutation behavior; editing algorithms opt in only for internal positions which must follow
// content as it is moved between containers. This corresponds to Gecko's RangeUpdater tracked points and the tracked
// Positions used by Blink and WebKit composite editing commands.
class MutationTrackedRange {
    AK_MAKE_NONCOPYABLE(MutationTrackedRange);
    AK_MAKE_NONMOVABLE(MutationTrackedRange);

public:
    explicit MutationTrackedRange(GC::Ref<DOM::Range>);
    ~MutationTrackedRange();

    DOM::Range& range() { return *m_range; }
    DOM::Range const& range() const { return *m_range; }

    static HashTable<DOM::Range*>& ranges();

private:
    GC::Root<DOM::Range> m_range;
};

}
