/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Media {

struct DecoderCapabilities {
    bool smooth { false };
    bool power_efficient { false };

    bool operator==(DecoderCapabilities const&) const = default;
};

}
