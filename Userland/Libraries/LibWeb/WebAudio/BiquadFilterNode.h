/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/BiquadFilterNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#BiquadFilterOptions
struct BiquadFilterOptions : AudioNodeOptions {
    Bindings::BiquadFilterType type { Bindings::BiquadFilterType::Lowpass };
    float q { 1 };
    float detune { 0 };
    float frequency { 350 };
    float gain { 0 };
};

// https://webaudio.github.io/web-audio-api/#BiquadFilterNode
class BiquadFilterNode : public AudioNode {
    WEB_PLATFORM_OBJECT(BiquadFilterNode, AudioNode);
    JS_DECLARE_ALLOCATOR(BiquadFilterNode);

public:
    virtual ~BiquadFilterNode() override;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> create(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, BiquadFilterOptions const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> construct_impl(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, BiquadFilterOptions const& = {});

protected:
    BiquadFilterNode(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, BiquadFilterOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
