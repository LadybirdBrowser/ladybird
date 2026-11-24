/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Diagnostics.h>
#include <sys/socket.h>

namespace Sandbox {

// The header of the control message that follows header in message, or null after the last one. musl's CMSG_NXTHDR()
// compares a size_t with a long, which -Wsign-compare rejects wherever the macro expands.
inline cmsghdr* next_control_message_header(msghdr& message, cmsghdr* header)
{
    AK_IGNORE_DIAGNOSTIC("-Wsign-compare", auto* next = CMSG_NXTHDR(&message, header));
    return next;
}

}
