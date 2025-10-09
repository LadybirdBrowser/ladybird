/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::EncryptedMediaExtensions {

class NavigatorEncryptedMediaExtensionsPartial {
public:
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> request_media_key_system_access(Utf16String const&, Vector<Bindings::MediaKeySystemConfiguration> const&);

private:
    virtual ~NavigatorEncryptedMediaExtensionsPartial() = default;

    friend class HTML::Navigator;
};

}
