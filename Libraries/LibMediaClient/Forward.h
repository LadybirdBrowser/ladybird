/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace MediaClient {

// Coded bytes ride inline in messages, in pieces well under the transport's message size limit.
static constexpr size_t MAX_CODED_BYTES_PER_MESSAGE = 4 * MiB;

class Client;
class RemoteMediaStream;
class RemotePlaybackManager;
class RemoteSourceBuffer;

}
