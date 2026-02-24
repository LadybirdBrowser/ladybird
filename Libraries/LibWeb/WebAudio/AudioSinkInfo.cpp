/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioSinkInfo.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioSinkInfo);

GC::Ref<AudioSinkInfo> AudioSinkInfo::create(JS::Realm& realm, Bindings::AudioSinkType type)
{
    return realm.create<AudioSinkInfo>(realm, type);
}

AudioSinkInfo::AudioSinkInfo(JS::Realm& realm, Bindings::AudioSinkType type)
    : PlatformObject(realm)
    , m_type(type)
{
}

void AudioSinkInfo::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioSinkInfo);
    Base::initialize(realm);
}

void AudioSinkInfo::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
