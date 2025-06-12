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
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class AnimationFrameCallbackDriver final : public JS::Cell {
    GC_CELL(AnimationFrameCallbackDriver, JS::Cell);
    GC_DECLARE_ALLOCATOR(AnimationFrameCallbackDriver);

    using Callback = GC::Ref<GC::Function<void(double)>>;

public:
    [[nodiscard]] WebIDL::UnsignedLong add(Callback handler);
    bool remove(WebIDL::UnsignedLong);
    bool has_callbacks() const;
    void run(double now);

private:
    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#animation-frame-callback-identifier
    WebIDL::UnsignedLong m_animation_frame_callback_identifier { 0 };

    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_callbacks;
    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_executing_callbacks;
};

}
