/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioScheduledSourceNode
class AudioScheduledSourceNode : public AudioNode {
    WEB_PLATFORM_OBJECT(AudioScheduledSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(AudioScheduledSourceNode);

public:
    virtual ~AudioScheduledSourceNode() override;

    GC::Ptr<WebIDL::CallbackType> onended();
    void set_onended(GC::Ptr<WebIDL::CallbackType>);

    WebIDL::ExceptionOr<void> start(double when = 0);
    WebIDL::ExceptionOr<void> stop(double when = 0);

protected:
    AudioScheduledSourceNode(JS::Realm&, GC::Ref<BaseAudioContext>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
