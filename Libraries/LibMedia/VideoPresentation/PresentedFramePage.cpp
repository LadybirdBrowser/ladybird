/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/VideoPresentation/PresentedFramePage.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::PresentedFramePage const& page)
{
    return encoder.encode(page.buffer());
}

template<>
ErrorOr<Media::PresentedFramePage> decode(Decoder& decoder)
{
    auto buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return Media::PresentedFramePage::create(move(buffer));
}

}
