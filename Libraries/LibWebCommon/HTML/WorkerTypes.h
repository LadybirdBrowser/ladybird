/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWebCommon/Bindings/Request.h>
#include <LibWebCommon/Bindings/Worker.h>

namespace Web::HTML {

enum class AgentType : u8 {
    SimilarOriginWindow,
    DedicatedWorker,
    SharedWorker,
    ServiceWorker,
    Worklet,
};

using RequestCredentials = Bindings::RequestCredentials;

using WorkerType = Bindings::WorkerType;

}
