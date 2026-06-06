/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/EncryptedMediaExtensions/MediaKeySystemAccess.h>

namespace Web::EncryptedMediaExtensions {

GC_DEFINE_ALLOCATOR(MediaKeySystemAccess);

MediaKeySystemAccess::~MediaKeySystemAccess() = default;

MediaKeySystemAccess::MediaKeySystemAccess(Utf16String const& key_system, Bindings::MediaKeySystemConfiguration configuration, NonnullOwnPtr<KeySystem> cdm_implementation)
    : Bindings::Wrappable()
    , m_key_system(key_system)
    , m_configuration(move(configuration))
    , m_cdm_implementation(move(cdm_implementation))
{
}

GC::Ref<MediaKeySystemAccess> MediaKeySystemAccess::create(Utf16String const& key_system, Bindings::MediaKeySystemConfiguration configuration, NonnullOwnPtr<KeySystem> cdm_implementation)
{
    return GC::Heap::the().allocate<MediaKeySystemAccess>(key_system, move(configuration), move(cdm_implementation));
}

}
