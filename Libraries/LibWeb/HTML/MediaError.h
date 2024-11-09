/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class MediaError final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaError, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(MediaError);

public:
    enum class Code : u16 {
        Aborted = 1,
        Network = 2,
        Decode = 3,
        SrcNotSupported = 4,
    };

    Code code() const { return m_code; }
    String const& message() const { return m_message; }

private:
    MediaError(JS::Realm&, Code code, String message);

    virtual void initialize(JS::Realm&) override;

    // https://html.spec.whatwg.org/multipage/media.html#dom-mediaerror-code
    Code m_code;

    // https://html.spec.whatwg.org/multipage/media.html#dom-mediaerror-message
    String m_message;
};

}
