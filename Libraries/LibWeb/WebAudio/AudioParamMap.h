/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioParamMap
class WEB_API AudioParamMap final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(AudioParamMap, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(AudioParamMap);

public:
    static GC::Ref<AudioParamMap> create();

    virtual ~AudioParamMap() override;

    OrderedHashMap<FlyString, GC::Ref<AudioParam>> const& entries() const { return m_entries; }
    void set_entry(FlyString key, GC::Ref<AudioParam> value) { m_entries.set(move(key), value); }

private:
    AudioParamMap();

    virtual void visit_edges(Cell::Visitor&) override;

    OrderedHashMap<FlyString, GC::Ref<AudioParam>> m_entries;
};

}
