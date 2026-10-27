/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ConservativeHashMap.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>
#include <LibGC/RootVector.h>
#include <LibWeb/UIEvents/TouchEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(TouchEvent);

GC::Ref<TouchEvent> TouchEvent::create(Utf16String const& type, Bindings::TouchEventInit const& init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<TouchEvent>(Utf16FlyString::from_utf16(type.utf16_view()), init, time_stamp);
}

GC::Ref<TouchEvent> TouchEvent::create(FlyString const& type, Bindings::TouchEventInit const& init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<TouchEvent>(Utf16FlyString::from_utf8(type.to_string()), init, time_stamp);
}

TouchEvent::TouchEvent(Utf16FlyString const& type, Bindings::TouchEventInit const& init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : UIEvent(type, init, time_stamp)
    , m_touches(TouchList::create(init.touches))
    , m_target_touches(TouchList::create(init.target_touches))
    , m_changed_touches(TouchList::create(init.changed_touches))
    , m_original_touches(m_touches)
    , m_original_target_touches(m_target_touches)
    , m_original_changed_touches(m_changed_touches)
    , m_alt_key(init.alt_key)
    , m_meta_key(init.meta_key)
    , m_ctrl_key(init.ctrl_key)
    , m_shift_key(init.shift_key)
{
    for (auto list : { m_touches, m_target_touches, m_changed_touches }) {
        for (u32 index = 0; index < list->length(); ++index)
            touch_target_list().append(list->item(index)->target());
    }
    m_last_touch_targets = touch_target_list();
}

// https://dom.spec.whatwg.org/#concept-event-dispatch
// NB: Each event path entry has retargeted touch targets. Clone changed Touch objects so that snapshots retained by
//     another listener remain immutable, and reuse each clone across the three lists.
void TouchEvent::retarget_touch_lists()
{
    auto const& targets = touch_target_list();
    if (targets == m_last_touch_targets)
        return;
    m_last_touch_targets = targets;
    GC::ConservativeHashMap<Touch*, GC::Ref<Touch>> clones;
    GC::RootVector<GC::Ref<Touch>> cloned_touches;
    size_t offset = 0;
    auto retarget = [&](GC::Ref<TouchList> original) {
        GC::ConservativeVector<GC::Ref<Touch>> touches;
        bool changed = false;
        for (u32 index = 0; index < original->length(); ++index) {
            auto touch = original->item(index);
            auto target = targets[offset++];
            if (target == touch->target()) {
                touches.append(*touch);
                continue;
            }
            changed = true;
            auto clone = clones.ensure(touch.ptr(), [&] {
                auto clone = touch->clone_with_target(*target);
                cloned_touches.append(clone);
                return clone;
            });
            touches.append(clone);
        }
        return changed ? TouchList::create(move(touches)) : original;
    };
    m_touches = retarget(m_original_touches);
    m_target_touches = retarget(m_original_target_touches);
    m_changed_touches = retarget(m_original_changed_touches);
}

void TouchEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_touches);
    visitor.visit(m_target_touches);
    visitor.visit(m_changed_touches);
    visitor.visit(m_original_touches);
    visitor.visit(m_original_target_touches);
    visitor.visit(m_original_changed_touches);
    visitor.visit(m_last_touch_targets);
}

}
