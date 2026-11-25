/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/MediaError.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::HTML {

class MediaError final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(MediaError, media_error)

public:
    enum class Code : u16 {
        Aborted = 1,
        Network = 2,
        Decode = 3,
        SrcNotSupported = 4,
    };

    [[nodiscard]] static GC::Ref<MediaError> create(Code, Utf16String const& message);

    Code code() const;
    Utf16String message() const;
};

}
