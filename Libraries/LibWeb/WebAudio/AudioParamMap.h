/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioParamMapPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebAudio {

class AudioParam;

// https://webaudio.github.io/web-audio-api/#audioparammap
class AudioParamMap final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioParamMap, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioParamMap);

public:
    [[nodiscard]] static GC::Ref<AudioParamMap> create(JS::Realm&);

    virtual ~AudioParamMap() override;

    size_t size() const;
    GC::Ptr<AudioParam> get(String const& key);
    bool has(String const& key);

    void set(FlyString key, GC::Ref<AudioParam> value);

    using ForEachCallback = Function<JS::ThrowCompletionOr<void>(FlyString const&, GC::Ref<AudioParam>)>;
    JS::ThrowCompletionOr<void> for_each(ForEachCallback);

private:
    friend class AudioParamMapIterator;

    explicit AudioParamMap(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    struct Entry {
        FlyString key;
        GC::Ref<AudioParam> value;
    };

    Vector<Entry> m_entries;
};

}
