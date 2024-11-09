/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>

namespace Gfx {

class ShareableBitmap {
public:
    ShareableBitmap() = default;

    enum Tag { ConstructWithKnownGoodBitmap };
    ShareableBitmap(NonnullRefPtr<Gfx::Bitmap>, Tag);

    bool is_valid() const { return m_bitmap; }

    Bitmap const* bitmap() const { return m_bitmap; }
    Bitmap* bitmap() { return m_bitmap; }

private:
    friend class Bitmap;

    RefPtr<Bitmap> m_bitmap;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ShareableBitmap const&);

template<>
ErrorOr<Gfx::ShareableBitmap> decode(Decoder&);

}
