/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Size.h>
#include <LibJS/Heap/Handle.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/images.html#image-request
class ImageRequest final : public JS::Cell {
    JS_CELL(ImageRequest, JS::Cell);
    JS_DECLARE_ALLOCATOR(ImageRequest);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ImageRequest> create(JS::Realm&, JS::NonnullGCPtr<Page>);

    ~ImageRequest();

    // https://html.spec.whatwg.org/multipage/images.html#img-req-state
    enum class State {
        Unavailable,
        PartiallyAvailable,
        CompletelyAvailable,
        Broken,
    };

    bool is_available() const;
    bool is_fetching() const;

    State state() const;
    void set_state(State);

    URL::URL const& current_url() const;
    void set_current_url(JS::Realm&, URL::URL);

    [[nodiscard]] JS::GCPtr<DecodedImageData> image_data() const;
    void set_image_data(JS::GCPtr<DecodedImageData>);

    [[nodiscard]] float current_pixel_density() const { return m_current_pixel_density; }
    void set_current_pixel_density(float density) { m_current_pixel_density = density; }

    [[nodiscard]] Optional<Gfx::FloatSize> const& preferred_density_corrected_dimensions() const { return m_preferred_density_corrected_dimensions; }
    void set_preferred_density_corrected_dimensions(Optional<Gfx::FloatSize> dimensions) { m_preferred_density_corrected_dimensions = move(dimensions); }

    // https://html.spec.whatwg.org/multipage/images.html#prepare-an-image-for-presentation
    void prepare_for_presentation(HTMLImageElement&);

    void fetch_image(JS::Realm&, JS::NonnullGCPtr<Fetch::Infrastructure::Request>);
    void add_callbacks(Function<void()> on_finish, Function<void()> on_fail);

    JS::GCPtr<SharedResourceRequest const> shared_resource_request() const { return m_shared_resource_request; }

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    explicit ImageRequest(JS::NonnullGCPtr<Page>);

    JS::NonnullGCPtr<Page> m_page;

    // https://html.spec.whatwg.org/multipage/images.html#img-req-state
    // An image request's state is initially unavailable.
    State m_state { State::Unavailable };

    // https://html.spec.whatwg.org/multipage/images.html#img-req-url
    // An image request's current URL is initially the empty string.
    URL::URL m_current_url;

    // https://html.spec.whatwg.org/multipage/images.html#img-req-data
    JS::GCPtr<DecodedImageData> m_image_data;

    // https://html.spec.whatwg.org/multipage/images.html#current-pixel-density
    // Each image request has a current pixel density, which must initially be 1.
    float m_current_pixel_density { 1 };

    // https://html.spec.whatwg.org/multipage/images.html#preferred-density-corrected-dimensions
    // Each image request has preferred density-corrected dimensions,
    // which is either a struct consisting of a width and a height or is null. It must initially be null.
    Optional<Gfx::FloatSize> m_preferred_density_corrected_dimensions;

    JS::GCPtr<SharedResourceRequest> m_shared_resource_request;
};

// https://html.spec.whatwg.org/multipage/images.html#abort-the-image-request
void abort_the_image_request(JS::Realm&, ImageRequest*);

}
