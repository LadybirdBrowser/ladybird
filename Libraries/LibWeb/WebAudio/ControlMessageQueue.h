/*
 * Copyright (c) 2025, Trey Shaffer <trey@trsh.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#control-message-queue
class ControlMessageQueue : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ControlMessageQueue, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ControlMessageQueue);

public:
    static GC::Ref<ControlMessageQueue> create(JS::Realm&);
    virtual ~ControlMessageQueue() override;

    void enqueue(GC::Ref<GC::Function<void()>>);
    void process_messages();

    bool has_messages() const;

private:
    explicit ControlMessageQueue(JS::Realm&);
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    mutable Threading::Mutex m_mutex;
    Vector<GC::Ref<GC::Function<void()>>> m_messages;
    bool m_is_processing { false };
};

}
