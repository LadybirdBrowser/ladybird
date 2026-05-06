/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Resource/Resource.h>
#include <LibPaintServer/Types.h>

namespace Web::Painting {

class DisplayListStreamSubmission {
public:
    virtual ~DisplayListStreamSubmission() = default;

    virtual ErrorOr<void> write_payload(size_t offset, ReadonlyBytes) = 0;
    virtual bool finish(PaintServer::FrameHeader const&, Vector<Gfx::ResourceTransfer>&&) = 0;
    virtual void abort() = 0;
};

using DisplayListStreamSink = Function<OwnPtr<DisplayListStreamSubmission>()>;

}
