/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Encoding/TextEncoderCommon.h>

namespace Web::Encoding {

TextEncoderCommonMixin::TextEncoderCommonMixin() = default;
TextEncoderCommonMixin::~TextEncoderCommonMixin() = default;

// https://encoding.spec.whatwg.org/#dom-textencoder-encoding
FlyString const& TextEncoderCommonMixin::encoding() const
{
    // The encoding getter steps are to return "utf-8".
    static FlyString const encoding = "utf-8"_fly_string;
    return encoding;
}

}
