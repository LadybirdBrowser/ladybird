/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibGfx/Forward.h>

namespace Media {

class TimedImage final {
public:
    TimedImage(AK::Duration timestamp, NonnullRefPtr<Gfx::Bitmap>&& image)
        : m_timestamp(timestamp)
        , m_image(move(image))
    {
    }
    TimedImage() = default;

    bool is_valid() const
    {
        return m_image != nullptr;
    }
    AK::Duration const& timestamp() const
    {
        VERIFY(is_valid());
        return m_timestamp;
    }
    NonnullRefPtr<Gfx::Bitmap> image() const
    {
        VERIFY(is_valid());
        return *m_image;
    }
    NonnullRefPtr<Gfx::Bitmap> release_image()
    {
        VERIFY(is_valid());
        m_timestamp = AK::Duration::zero();
        return m_image.release_nonnull();
    }

private:
    AK::Duration m_timestamp { AK::Duration::zero() };
    RefPtr<Gfx::Bitmap> m_image { nullptr };
};

}
