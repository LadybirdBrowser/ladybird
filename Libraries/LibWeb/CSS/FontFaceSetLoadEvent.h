/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Event.h>

namespace Web::CSS {

struct FontFaceSetLoadEventInit : public DOM::EventInit {
    Vector<GC::Root<FontFace>> fontfaces;
};

class FontFaceSetLoadEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(FontFaceSetLoadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(FontFaceSetLoadEvent);

public:
    [[nodiscard]] static GC::Ref<FontFaceSetLoadEvent> create(JS::Realm&, FlyString const& type, FontFaceSetLoadEventInit const& event_init = {});
    static WebIDL::ExceptionOr<GC::Ref<FontFaceSetLoadEvent>> construct_impl(JS::Realm&, FlyString const& type, FontFaceSetLoadEventInit const& event_init = {});

    virtual ~FontFaceSetLoadEvent() override = default;

    Vector<GC::Ref<FontFace>> const& fontfaces() const { return m_fontfaces; }

private:
    FontFaceSetLoadEvent(JS::Realm&, FlyString const& type, FontFaceSetLoadEventInit const& event_init = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    Vector<GC::Ref<FontFace>> m_fontfaces;
};

}
