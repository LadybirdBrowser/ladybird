/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::Fetch::Engine {

// Who consumes the response body.
enum class BodyIntent : u8 {
    DeliverStreamToConsumerAgent,
    ConsumeForCallback,
    DrainAndDiscard,
    SyncAccumulate,
};

}
