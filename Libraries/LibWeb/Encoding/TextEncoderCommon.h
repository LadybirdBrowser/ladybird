/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>

namespace Web::Encoding {

class TextEncoderCommonMixin {
public:
    virtual ~TextEncoderCommonMixin();

    // https://encoding.spec.whatwg.org/#dom-textencoder-encoding
    Utf16FlyString const& encoding() const;

protected:
    TextEncoderCommonMixin();
};

}
