/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioDestinationNode
class AudioDestinationNode : public AudioNode {
    WEB_PLATFORM_OBJECT(AudioDestinationNode, AudioNode);
    JS_DECLARE_ALLOCATOR(AudioDestinationNode);

public:
    virtual ~AudioDestinationNode() override;

protected:
    AudioDestinationNode(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
