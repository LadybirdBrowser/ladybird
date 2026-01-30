/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebAudio/AudioParamMap.h>

namespace Web::WebAudio {

class AudioParamMapIterator final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioParamMapIterator, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioParamMapIterator);

public:
    [[nodiscard]] static GC::Ref<AudioParamMapIterator> create(AudioParamMap const&, JS::Object::PropertyKind iteration_kind);

    virtual ~AudioParamMapIterator() override;

    JS::Object* next();

private:
    AudioParamMapIterator(AudioParamMap const&, JS::Object::PropertyKind iteration_kind);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<AudioParamMap const> m_map;
    JS::Object::PropertyKind m_iteration_kind;
    size_t m_index { 0 };
};

}
