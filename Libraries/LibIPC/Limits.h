/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace IPC {

// Maximum size of an IPC message payload (64 MiB should be more than enough)
static constexpr size_t MAX_MESSAGE_PAYLOAD_SIZE = 64 * MiB;

// Maximum number of file descriptors per message
static constexpr size_t MAX_MESSAGE_FD_COUNT = 128;

}
