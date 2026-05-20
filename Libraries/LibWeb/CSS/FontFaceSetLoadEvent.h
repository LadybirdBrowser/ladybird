/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Event.h>

namespace Web::CSS {

class FontFaceSetLoadEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(FontFaceSetLoadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(FontFaceSetLoadEvent);

public:
    [[nodiscard]] static GC::Ref<FontFaceSetLoadEvent> create(JS::Realm&, FlyString const& type, Bindings::FontFaceSetLoadEventInit const&);
    static WebIDL::ExceptionOr<GC::Ref<FontFaceSetLoadEvent>> construct_impl(JS::Realm&, FlyString const& type, Bindings::FontFaceSetLoadEventInit const&);

    virtual ~FontFaceSetLoadEvent() override = default;

    Vector<GC::Ref<FontFace>> const& fontfaces() const { return m_fontfaces; }

private:
    FontFaceSetLoadEvent(JS::Realm&, FlyString const& type, Bindings::FontFaceSetLoadEventInit const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    Vector<GC::Ref<FontFace>> m_fontfaces;
};

}
