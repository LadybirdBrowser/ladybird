/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaKeySystemAccessPrototype.h>
#include <LibWeb/EncryptedMediaExtensions/MediaKeySystemAccess.h>

namespace Web::EncryptedMediaExtensions {

GC_DEFINE_ALLOCATOR(MediaKeySystemAccess);

MediaKeySystemAccess::~MediaKeySystemAccess() = default;

MediaKeySystemAccess::MediaKeySystemAccess(JS::Realm& realm, WebIDL::DOMString key_system, Bindings::MediaKeySystemConfiguration configuration, RefPtr<KeySystem> cdm_implementation)
    : PlatformObject(realm)
    , m_key_system(key_system)
    , m_configuration(configuration)
    , m_cdm_implementation(cdm_implementation)
{
}

GC::Ref<MediaKeySystemAccess> MediaKeySystemAccess::create(JS::Realm& realm, WebIDL::DOMString key_system, Bindings::MediaKeySystemConfiguration configuration, RefPtr<KeySystem> cdm_implementation)
{
    return realm.create<MediaKeySystemAccess>(realm, key_system, configuration, cdm_implementation);
}

void MediaKeySystemAccess::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaKeySystemAccess);
    Base::initialize(realm);
}

}
