/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

class MediaError final : public Bindings::Wrappable {
    WEB_WRAPPABLE(MediaError, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MediaError);

public:
    enum class Code : u16 {
        Aborted = 1,
        Network = 2,
        Decode = 3,
        SrcNotSupported = 4,
    };

    [[nodiscard]] static GC::Ref<MediaError> create(Code, String message);

    Code code() const { return m_code; }
    String const& message() const { return m_message; }

private:
    MediaError(Code code, String message);

    // https://html.spec.whatwg.org/multipage/media.html#dom-mediaerror-code
    Code m_code;

    // https://html.spec.whatwg.org/multipage/media.html#dom-mediaerror-message
    String m_message;
};

}
