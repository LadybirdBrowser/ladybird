/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#dom-sourcebufferlist
class SourceBufferList : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SourceBufferList, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SourceBufferList);

public:
    void append(GC::Ref<SourceBuffer>);

    size_t length() const;
    GC::Ref<SourceBuffer> const& item(u32 index) const;

    void set_onaddsourcebuffer(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onaddsourcebuffer();

    void set_onremovesourcebuffer(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onremovesourcebuffer();

    bool contains(SourceBuffer const&) const;

private:
    SourceBufferList(JS::Realm&);

    virtual ~SourceBufferList() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<GC::Ref<SourceBuffer>> m_buffers;
};

}
