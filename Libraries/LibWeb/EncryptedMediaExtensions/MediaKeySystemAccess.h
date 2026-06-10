/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>
#include <LibWeb/EncryptedMediaExtensions/KeySystem.h>

namespace Web::EncryptedMediaExtensions {

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemaccess
class MediaKeySystemAccess : public Bindings::Wrappable {
    WEB_WRAPPABLE(MediaKeySystemAccess, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MediaKeySystemAccess);

public:
    virtual ~MediaKeySystemAccess() override;
    [[nodiscard]] static GC::Ref<MediaKeySystemAccess> create(Utf16String const&, MediaKeySystemConfiguration, NonnullOwnPtr<KeySystem>);

    [[nodiscard]] Utf16String key_system() const { return m_key_system; }
    [[nodiscard]] MediaKeySystemConfiguration const& get_configuration() const { return m_configuration; }

protected:
    explicit MediaKeySystemAccess(Utf16String const&, MediaKeySystemConfiguration, NonnullOwnPtr<KeySystem>);

private:
    Utf16String m_key_system;

    MediaKeySystemConfiguration m_configuration;
    NonnullOwnPtr<KeySystem> m_cdm_implementation;
};

}
