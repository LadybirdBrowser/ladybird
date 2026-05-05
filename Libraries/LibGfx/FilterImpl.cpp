/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/FilterImpl.h>

namespace Gfx {

namespace {

void append_unique_image_reference(Vector<FilterImageReference>& image_references, FilterImageReference const& image_reference)
{
    for (auto const& existing_reference : image_references) {
        if (existing_reference.skia_image_unique_id == image_reference.skia_image_unique_id)
            return;
    }
    MUST(image_references.try_append(image_reference));
}

void append_image_references(Vector<FilterImageReference>& image_references, ReadonlySpan<FilterImageReference const> additional_references)
{
    MUST(image_references.try_ensure_capacity(image_references.size() + additional_references.size()));
    for (auto const& image_reference : additional_references)
        append_unique_image_reference(image_references, image_reference);
}

void append_image_references(Vector<FilterImageReference>& image_references, Optional<Filter const&> filter)
{
    if (!filter.has_value())
        return;
    append_image_references(image_references, filter->impl().image_references.span());
}

}

NonnullOwnPtr<FilterImpl> FilterImpl::create(sk_sp<SkImageFilter> filter)
{
    return adopt_own(*new FilterImpl { move(filter), {} });
}

NonnullOwnPtr<FilterImpl> FilterImpl::create(sk_sp<SkImageFilter> filter, FilterImageReference&& image_reference)
{
    Vector<FilterImageReference> image_references;
    MUST(image_references.try_append(move(image_reference)));
    return create(move(filter), move(image_references));
}

NonnullOwnPtr<FilterImpl> FilterImpl::create(sk_sp<SkImageFilter> filter, Optional<Filter const&> image_reference_source)
{
    Vector<FilterImageReference> image_references;
    append_image_references(image_references, image_reference_source);
    return create(move(filter), move(image_references));
}

NonnullOwnPtr<FilterImpl> FilterImpl::create(sk_sp<SkImageFilter> filter, Optional<Filter const&> first_image_reference_source, Optional<Filter const&> second_image_reference_source)
{
    Vector<FilterImageReference> image_references;
    append_image_references(image_references, first_image_reference_source);
    append_image_references(image_references, second_image_reference_source);
    return create(move(filter), move(image_references));
}

NonnullOwnPtr<FilterImpl> FilterImpl::create(sk_sp<SkImageFilter> filter, ReadonlySpan<Optional<Filter> const> image_reference_sources)
{
    Vector<FilterImageReference> image_references;
    for (auto const& image_reference_source : image_reference_sources)
        append_image_references(image_references, image_reference_source);
    return create(move(filter), move(image_references));
}

NonnullOwnPtr<FilterImpl> FilterImpl::create(sk_sp<SkImageFilter> filter, Vector<FilterImageReference>&& image_references)
{
    return adopt_own(*new FilterImpl { move(filter), move(image_references) });
}

NonnullOwnPtr<FilterImpl> FilterImpl::clone() const
{
    return adopt_own(*new FilterImpl { filter, image_references });
}

}
