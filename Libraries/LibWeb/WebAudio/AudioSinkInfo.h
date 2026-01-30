/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioSinkInfoPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebAudio {

class AudioSinkInfo final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioSinkInfo, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioSinkInfo);

public:
    static GC::Ref<AudioSinkInfo> create(JS::Realm&, Bindings::AudioSinkType);

    virtual ~AudioSinkInfo() override = default;

    Bindings::AudioSinkType type() const { return m_type; }

private:
    AudioSinkInfo(JS::Realm&, Bindings::AudioSinkType);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Bindings::AudioSinkType m_type { Bindings::AudioSinkType::None };
};

}
