/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

using AutomationRate = Bindings::AutomationRate;

class AudioParam final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AudioParam, audio_param)

public:
    enum class FixedAutomationRate : u8 { No,
        Yes };
    static GC::Ref<AudioParam> create(GC::Ref<BaseAudioContext>, GC::Ptr<AudioNode> owner, float default_value, float min_value, float max_value, AutomationRate, FixedAutomationRate = FixedAutomationRate::No);
    GC::Ref<BaseAudioContext> context() const;
    WebIDL::ExceptionOr<void> set_value(float);
    float intrinsic_value_at_time(double time) const;
    NonnullRefPtr<Rendering::RenderAudioParam> render_param() const;
};

}
