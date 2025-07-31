/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>
#include <LibWeb/EncryptedMediaExtensions/KeySystem.h>

namespace Web::EncryptedMediaExtensions {

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemaccess
class MediaKeySystemAccess : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaKeySystemAccess, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaKeySystemAccess);

public:
    virtual ~MediaKeySystemAccess() override;
    [[nodiscard]] static GC::Ref<MediaKeySystemAccess> create(JS::Realm&, String, Bindings::MediaKeySystemConfiguration, RefPtr<KeySystem>);

    [[nodiscard]] String key_system() const { return m_key_system; }
    [[nodiscard]] Bindings::MediaKeySystemConfiguration get_configuration() const { return m_configuration; }

protected:
    explicit MediaKeySystemAccess(JS::Realm&, String, Bindings::MediaKeySystemConfiguration, RefPtr<KeySystem>);
    virtual void initialize(JS::Realm&) override;

private:
    String m_key_system;

    Bindings::MediaKeySystemConfiguration m_configuration;
    RefPtr<KeySystem> m_cdm_implementation;
};

}
