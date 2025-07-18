/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TextTrackPrototype.h>
#include <LibWeb/HTML/TextTrack.h>

namespace Web::HTML {

class TextTrackObserver final : public Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(TextTrackObserver, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TextTrackObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] GC::Ptr<GC::Function<void(TextTrack::ReadinessState)>> track_readiness_observer() const { return m_track_readiness_observer; }
    void set_track_readiness_observer(Function<void(TextTrack::ReadinessState)>);

private:
    explicit TextTrackObserver(JS::Realm&, TextTrack&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    GC::Ref<TextTrack> m_text_track;
    GC::Ptr<GC::Function<void(TextTrack::ReadinessState)>> m_track_readiness_observer;
};

}
