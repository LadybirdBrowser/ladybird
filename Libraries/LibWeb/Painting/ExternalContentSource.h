/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibThreading/Mutex.h>

namespace Web::Painting {

class ExternalContentSource final : public AtomicRefCounted<ExternalContentSource> {
public:
    static NonnullRefPtr<ExternalContentSource> create();

    void update(Optional<Gfx::DecodedImageFrame>);
    void clear();
    Optional<Gfx::DecodedImageFrame> current_frame() const;

private:
    ExternalContentSource() = default;

    mutable Threading::Mutex m_mutex;
    Optional<Gfx::DecodedImageFrame> m_frame;
};

}
