/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/FontFaceSetLoadEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::CSS {

class FontFace;

using FontFaceSetLoadEventInit = Bindings::FontFaceSetLoadEventInit;

class FontFaceSetLoadEvent : public DOM::Event {
    WEB_WRAPPABLE(FontFaceSetLoadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(FontFaceSetLoadEvent);

public:
    [[nodiscard]] static GC::Ref<FontFaceSetLoadEvent> create(FlyString const& type, FontFaceSetLoadEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~FontFaceSetLoadEvent() override = default;

    Vector<GC::Ref<FontFace>> const& fontfaces() const { return m_fontfaces; }

private:
    FontFaceSetLoadEvent(FlyString const& type, FontFaceSetLoadEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(Visitor&) override;

    Vector<GC::Ref<FontFace>> m_fontfaces;
};

}
