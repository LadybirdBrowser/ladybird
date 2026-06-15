/*
 * Copyright (c) 2023-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/images.html#img-req-data
class DecodedImageData : public JS::Cell {
    GC_CELL(DecodedImageData, JS::Cell);
    friend class Web::Internals::Internals;

public:
    class Client {
    public:
        virtual GC::Ptr<DecodedImageData> decoded_image_data() const = 0;
        virtual void decoded_image_data_did_update() = 0;

    protected:
        void register_with_decoded_image_data_if_needed();
        void unregister_with_decoded_image_data_if_needed();
    };

    virtual ~DecodedImageData();

    [[nodiscard]] bool is_cors_cross_origin() const { return m_is_cors_cross_origin; }
    void set_is_cors_cross_origin(bool value) { m_is_cors_cross_origin = value; }

    virtual void paint([[maybe_unused]] DisplayListRecordingContext&, [[maybe_unused]] Gfx::IntRect dst_rect, CSS::ImageRendering) const = 0;

    virtual Optional<Gfx::DecodedImageFrame> default_frame(Gfx::IntSize = {}) const = 0;
    virtual Optional<Gfx::DecodedImageFrame> current_frame(Gfx::IntSize = {}) const = 0;

    virtual void restart_animation() { }

    virtual Optional<CSSPixels> intrinsic_width() const = 0;
    virtual Optional<CSSPixels> intrinsic_height() const = 0;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const = 0;

protected:
    DecodedImageData();

    void notify_clients_did_update();
    bool has_clients() const { return !m_clients.is_empty(); }
    virtual void on_client_registered() { }

private:
    HashTable<Client*> m_clients;
    bool m_is_cors_cross_origin { false };
};

}
