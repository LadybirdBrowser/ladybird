/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Types.h>
#include <LibMediaCodecs/TrackType.h>

namespace Media {

// Indicates which decoder was selected for a codec, so that the selection can be compared upon a config change before
// tearing down an existing decoder. Each kind of track indexes its own registry, so their selections are distinct
// types and cannot be exchanged.
template<TrackType track_type>
class DecoderSelection {
public:
    DecoderSelection() = default;
    explicit DecoderSelection(i32 registration_index)
        : m_registration_index(registration_index)
    {
        VERIFY(registration_index >= 0);
    }

    bool has_value() const { return m_registration_index >= 0; }
    i32 registration_index() const { return m_registration_index; }

    bool operator==(DecoderSelection const&) const = default;

private:
    i32 m_registration_index { -1 };
};

using AudioDecoderSelection = DecoderSelection<TrackType::Audio>;
using VideoDecoderSelection = DecoderSelection<TrackType::Video>;

}
