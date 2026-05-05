/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

struct PendingCompletion {
    SurfaceID surface_id { 0 };
    ReleaseToken release_token { 0 };
};

struct IngressFrame final : public RefCounted<IngressFrame> {
    PaintServer::FrameHeader header;
    ByteBuffer payload;
    SyncToken wait_sync_token { 0 };
    SyncToken render_wait_sync_token { 0 };
    ReleaseToken release_token { 0 };
};

}
