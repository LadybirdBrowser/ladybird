/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MediaKeySystemAccess.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::EncryptedMediaExtensions {

class MediaKeySystemAccess : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(MediaKeySystemAccess, media_key_system_access)
};

}
