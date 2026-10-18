/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibGC/RootVector.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/TextTrackCue.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/media.html#texttrackcuelist
class TextTrackCueList final : public DOM::EventTarget {
    WEB_WRAPPABLE(TextTrackCueList, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(TextTrackCueList);

public:
    enum class Subset {
        All,
        Active,
    };

    static GC::Ref<TextTrackCueList> create(GC::Ref<TextTrack>, Subset);
    virtual ~TextTrackCueList() override;

    size_t length() const;

    GC::Ptr<TextTrackCue> item(size_t index) const;
    GC::Ptr<TextTrackCue> get_cue_by_id(Utf16View id) const;

private:
    TextTrackCueList(GC::Ref<TextTrack>, Subset);
    virtual void visit_edges(Visitor&) override;

    template<typename Callback>
    void for_each_cue(Callback) const;

    GC::Ref<TextTrack> m_track;
    Subset m_subset;
};

}
