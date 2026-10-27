/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/TouchEvent.h>
#include <LibWeb/UIEvents/TouchList.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

class TouchEvent final : public UIEvent {
    WEB_WRAPPABLE(TouchEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(TouchEvent);

public:
    static GC::Ref<TouchEvent> create(Utf16String const&, Bindings::TouchEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static GC::Ref<TouchEvent> create(FlyString const&, Bindings::TouchEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    void retarget_touch_lists();
    GC::Ref<TouchList> touches() const { return m_touches; }
    GC::Ref<TouchList> target_touches() const { return m_target_touches; }
    GC::Ref<TouchList> changed_touches() const { return m_changed_touches; }
    bool alt_key() const { return m_alt_key; }
    bool meta_key() const { return m_meta_key; }
    bool ctrl_key() const { return m_ctrl_key; }
    bool shift_key() const { return m_shift_key; }

private:
    TouchEvent(Utf16FlyString const&, Bindings::TouchEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<TouchList> m_touches;
    GC::Ref<TouchList> m_target_touches;
    GC::Ref<TouchList> m_changed_touches;
    GC::Ref<TouchList> m_original_touches;
    GC::Ref<TouchList> m_original_target_touches;
    GC::Ref<TouchList> m_original_changed_touches;
    TouchTargetList m_last_touch_targets;
    bool m_alt_key { false };
    bool m_meta_key { false };
    bool m_ctrl_key { false };
    bool m_shift_key { false };
};

}
