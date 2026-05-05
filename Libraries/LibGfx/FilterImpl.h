/*
 * Copyright (c) 2024-2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/DecodedImageFrame.h>
#include <core/SkColorFilter.h>
#include <effects/SkImageFilters.h>

namespace Gfx {

class Filter;

struct FilterImageReference {
    u32 skia_image_unique_id { 0 };
    NonnullRefPtr<DecodedImageFrame const> frame;
};

struct FilterImpl {
    sk_sp<SkImageFilter> filter;
    Vector<FilterImageReference> image_references;

    static NonnullOwnPtr<FilterImpl> create(sk_sp<SkImageFilter> filter);
    static NonnullOwnPtr<FilterImpl> create(sk_sp<SkImageFilter> filter, FilterImageReference&& image_reference);
    static NonnullOwnPtr<FilterImpl> create(sk_sp<SkImageFilter> filter, Optional<Filter const&> image_reference_source);
    static NonnullOwnPtr<FilterImpl> create(sk_sp<SkImageFilter> filter, Optional<Filter const&> first_image_reference_source, Optional<Filter const&> second_image_reference_source);
    static NonnullOwnPtr<FilterImpl> create(sk_sp<SkImageFilter> filter, ReadonlySpan<Optional<Filter> const> image_reference_sources);
    static NonnullOwnPtr<FilterImpl> create(sk_sp<SkImageFilter> filter, Vector<FilterImageReference>&& image_references);

    NonnullOwnPtr<FilterImpl> clone() const;
};

}
