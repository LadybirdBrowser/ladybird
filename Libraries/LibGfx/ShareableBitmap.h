/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGfx/Export.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>

namespace Gfx {

class GFX_API ShareableBitmap {
public:
    ShareableBitmap();

    enum Tag { ConstructWithKnownGoodBitmap };
    ShareableBitmap(NonnullRefPtr<Gfx::Bitmap>, Tag);

    ~ShareableBitmap();

    ShareableBitmap(ShareableBitmap const&);
    ShareableBitmap(ShareableBitmap&&);

    ShareableBitmap& operator=(ShareableBitmap const&);
    ShareableBitmap& operator=(ShareableBitmap&&);

    bool is_valid() const;

    Bitmap const* bitmap() const;
    Bitmap* bitmap();

private:
    friend class Bitmap;

    RefPtr<Bitmap> m_bitmap;
};

}

namespace IPC {

template<>
GFX_API ErrorOr<void> encode(Encoder&, Gfx::ShareableBitmap const&);

template<>
GFX_API ErrorOr<Gfx::ShareableBitmap> decode(Decoder&);

}
