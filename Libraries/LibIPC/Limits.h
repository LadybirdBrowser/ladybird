/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NumericLimits.h>
#include <AK/Types.h>

namespace IPC {

// Maximum size of an IPC message payload
static constexpr size_t MAX_MESSAGE_PAYLOAD_SIZE = NumericLimits<u32>::max();

// Maximum number of file descriptors per message. Transports carry any number up to this, however few fit in one
// system call. It stays below the 16383 port rights that XNU accepts in one Mach message, so that a message is never
// split there, and far below the descriptor limit LibMain raises every process to.
static constexpr size_t MAX_MESSAGE_FD_COUNT = 16000;

// A transport that sends a payload in shared memory attaches one more descriptor to its message.
static constexpr size_t MAX_MESSAGE_FD_COUNT_WITH_SHARED_MEMORY = MAX_MESSAGE_FD_COUNT + 1;

}
