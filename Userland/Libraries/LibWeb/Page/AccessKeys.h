/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Runtime/WeakContainer.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DragDataStore.h>
#include <LibWeb/Page/AccessKeyNames.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/KeyCode.h>

namespace Web {

enum class AccessKey : u8 {
#define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) /* NOLINT(misc-confusable-identifiers) */ name,
    ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#undef __ENUMERATE_ACCESS_KEY
};

class AccessKeys : public JS::WeakContainer {
    AK_MAKE_NONCOPYABLE(AccessKeys);
    AK_MAKE_NONMOVABLE(AccessKeys);

public:
    AccessKeys(JS::Heap& heap)
        : WeakContainer(heap)
    {
    }

    static Optional<AccessKey> find_by_codepoint(u32);
    static Optional<AccessKey> find_by_keycode(UIEvents::KeyCode);
    static String label(AccessKey);

    void assign(DOM::Element&, AccessKey);
    void unassign(DOM::Element&);

    bool trigger_action(AccessKey) const;

    Optional<AccessKey> assigned_access_key(DOM::Element const&) const;

    virtual void remove_dead_cells(Badge<JS::Heap>) override;

private:
    HashMap<AccessKey, Vector<JS::RawGCPtr<DOM::Element>>> m_assigned_access_key;
};

}
