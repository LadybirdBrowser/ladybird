/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/TextTrackObserver.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextTrackObserver);

TextTrackObserver::TextTrackObserver(JS::Realm&, TextTrack& text_track)
    : m_text_track(text_track)
{
    m_text_track->register_observer({}, *this);
}

void TextTrackObserver::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_text_track);
    visitor.visit(m_track_readiness_observer);
}

void TextTrackObserver::finalize()
{
    Base::finalize();
    m_text_track->unregister_observer({}, *this);
}

void TextTrackObserver::set_track_readiness_observer(Function<void(TextTrack::ReadinessState)> callback)
{
    if (callback)
        m_track_readiness_observer = GC::create_function(m_text_track->realm().heap(), move(callback));
    else
        m_track_readiness_observer = nullptr;
}

}
