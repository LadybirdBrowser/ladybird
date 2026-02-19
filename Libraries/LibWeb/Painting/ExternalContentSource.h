/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/RefPtr.h>
#include <LibGfx/Forward.h>
#include <LibThreading/Mutex.h>

namespace Web::Painting {

class ExternalContentSource final : public AtomicRefCounted<ExternalContentSource> {
public:
    static NonnullRefPtr<ExternalContentSource> create();

    void update(RefPtr<Gfx::ImmutableBitmap>);
    void clear();
    RefPtr<Gfx::ImmutableBitmap> current_bitmap() const;

private:
    ExternalContentSource() = default;

    mutable Threading::Mutex m_mutex;
    RefPtr<Gfx::ImmutableBitmap> m_bitmap;
};

}
