/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

class WEB_API AnimationFrameCallbackDriver final : public JS::Cell {
    GC_CELL(AnimationFrameCallbackDriver, JS::Cell);
    GC_DECLARE_ALLOCATOR(AnimationFrameCallbackDriver);

    using Callback = GC::Ref<GC::Function<void(double)>>;

public:
    size_t callback_count() const { return m_callbacks.size() + m_after_callbacks.size(); }
    [[nodiscard]] static GC::Ref<AnimationFrameCallbackDriver> create();

    [[nodiscard]] WebIDL::UnsignedLong add(Callback handler);
    [[nodiscard]] WebIDL::UnsignedLong add_after_callbacks(Callback handler);
    bool remove(WebIDL::UnsignedLong);
    bool has_callbacks() const;
    void run(double now);
    void run_after_callbacks(double now);

private:
    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#animation-frame-callback-identifier
    WebIDL::UnsignedLong m_animation_frame_callback_identifier { 0 };

    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_callbacks;
    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_executing_callbacks;
    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_after_callbacks;
    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_executing_after_callbacks;
};

}
