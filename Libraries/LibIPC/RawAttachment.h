/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Attachment.h>

namespace IPC {

// A Mach port name and its disposition on Apple, and a descriptor elsewhere.
struct RawAttachment {
    int value;
    u32 disposition;
};

Vector<Attachment> adopt_raw_attachments(ReadonlySpan<RawAttachment>);
Vector<RawAttachment> release_raw_attachments(Vector<Attachment>);

}
