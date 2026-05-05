/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Painting/ExternalContentSource.h>

namespace Web::HTML {

DecodedImageData::DecodedImageData() = default;

DecodedImageData::~DecodedImageData() = default;

RefPtr<Painting::ExternalContentSource> DecodedImageData::external_content_source(size_t, Gfx::IntSize) const
{
    return nullptr;
}

}
