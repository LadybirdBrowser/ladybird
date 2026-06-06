/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/EncryptedMediaExtensions/MediaKeySystemAccess.h>

namespace Web::EncryptedMediaExtensions {

GC_DEFINE_ALLOCATOR(MediaKeySystemAccess);

MediaKeySystemAccess::~MediaKeySystemAccess() = default;

MediaKeySystemAccess::MediaKeySystemAccess(JS::Realm& realm, Utf16String const& key_system, Bindings::MediaKeySystemConfiguration configuration, NonnullOwnPtr<KeySystem> cdm_implementation)
    : Wrappable(realm)
    , m_key_system(key_system)
    , m_configuration(move(configuration))
    , m_cdm_implementation(move(cdm_implementation))
{
}

GC::Ref<MediaKeySystemAccess> MediaKeySystemAccess::create(JS::Realm& realm, Utf16String const& key_system, Bindings::MediaKeySystemConfiguration configuration, NonnullOwnPtr<KeySystem> cdm_implementation)
{
    return realm.create<MediaKeySystemAccess>(realm, key_system, configuration, move(cdm_implementation));
}

}
