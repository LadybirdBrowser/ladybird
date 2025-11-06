/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::MediaSourceExtensions {

class SourceBuffer;

// https://w3c.github.io/media-source/#dom-sourcebufferlist
class SourceBufferList : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SourceBufferList, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SourceBufferList);

public:
    // WebIDL properties
    size_t length() const { return m_source_buffers.size(); }
    SourceBuffer* item(size_t index);

    void set_onaddsourcebuffer(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onaddsourcebuffer();

    void set_onremovesourcebuffer(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onremovesourcebuffer();

    // Internal methods
    void add(SourceBuffer&);
    void remove(SourceBuffer&);
    bool contains(SourceBuffer const&) const;
    void clear();

    virtual void visit_edges(Cell::Visitor&) override;

private:
    friend class MediaSource;

    SourceBufferList(JS::Realm&);

    virtual ~SourceBufferList() override;

    virtual void initialize(JS::Realm&) override;

    Vector<GC::Ref<SourceBuffer>> m_source_buffers;
};

}
