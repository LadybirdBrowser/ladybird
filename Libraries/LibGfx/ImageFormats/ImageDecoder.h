/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CMYKBitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Size.h>
#include <LibGfx/VectorGraphic.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

namespace Gfx {

class Bitmap;

struct ImageFrameDescriptor {
    RefPtr<Bitmap> image;
    int duration { 0 };
};

struct VectorImageFrameDescriptor {
    RefPtr<VectorGraphic> image;
    int duration { 0 };
};

class Metadata {
public:
    Metadata() = default;
    virtual ~Metadata() = default;

    HashMap<StringView, String> const& main_tags() const
    {
        if (m_main_tags.is_empty())
            fill_main_tags();

        // This is designed to be used in a general GUI, don't include too much information here.
        VERIFY(m_main_tags.size() < 8);

        return m_main_tags;
    }

protected:
    virtual void fill_main_tags() const { }

    mutable HashMap<StringView, String> m_main_tags;
};

enum class NaturalFrameFormat {
    RGB,
    Grayscale,
    CMYK,
    Vector,
};

class ImageDecoderPlugin {
public:
    virtual ~ImageDecoderPlugin() = default;

    // Each plugin should implement these static functions and register them in ImageDecoder.cpp
    // Implement sniff() if the file includes a magic number
    // static bool sniff(ReadonlyBytes);
    // Implement validate_before_create() otherwise
    // static ErrorOr<bool> validate_before_create(ReadonlyBytes);

    // This function should be used to both create the context and parse the image header.
    // static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);

    // This should always be available as gathered in create()
    virtual IntSize size() = 0;

    // Override this if the format supports animated images
    virtual bool is_animated() { return false; }
    virtual size_t loop_count() { return 0; }
    virtual size_t frame_count() { return 1; }
    virtual size_t first_animated_frame_index() { return 0; }

    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) = 0;

    virtual Optional<Metadata const&> metadata() { return OptionalNone {}; }

    virtual ErrorOr<Optional<Media::CodingIndependentCodePoints>> cicp() { return OptionalNone {}; }
    virtual ErrorOr<Optional<ReadonlyBytes>> icc_data() { return OptionalNone {}; }

    virtual NaturalFrameFormat natural_frame_format() const { return NaturalFrameFormat::RGB; }
    virtual ErrorOr<NonnullRefPtr<CMYKBitmap>> cmyk_frame() { VERIFY_NOT_REACHED(); }
    virtual ErrorOr<VectorImageFrameDescriptor> vector_frame(size_t) { VERIFY_NOT_REACHED(); }

protected:
    ImageDecoderPlugin() = default;
};

class ImageDecoder : public RefCounted<ImageDecoder> {
public:
    static ErrorOr<RefPtr<ImageDecoder>> try_create_for_raw_bytes(ReadonlyBytes, Optional<ByteString> mime_type = {});
    ~ImageDecoder() = default;

    IntSize size() const { return m_plugin->size(); }
    int width() const { return size().width(); }
    int height() const { return size().height(); }
    bool is_animated() const { return m_plugin->is_animated(); }
    size_t loop_count() const { return m_plugin->loop_count(); }
    size_t frame_count() const { return m_plugin->frame_count(); }
    size_t first_animated_frame_index() const { return m_plugin->first_animated_frame_index(); }

    ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) const { return m_plugin->frame(index, ideal_size); }

    Optional<Metadata const&> metadata() const { return m_plugin->metadata(); }
    ErrorOr<ColorSpace> color_space();
    ErrorOr<Optional<ReadonlyBytes>> icc_data() const { return m_plugin->icc_data(); }

    NaturalFrameFormat natural_frame_format() { return m_plugin->natural_frame_format(); }

    // Call only if natural_frame_format() == NaturalFrameFormat::CMYK.
    ErrorOr<NonnullRefPtr<CMYKBitmap>> cmyk_frame() { return m_plugin->cmyk_frame(); }

    // Call only if natural_frame_format() == NaturalFrameFormat::Vector.
    ErrorOr<VectorImageFrameDescriptor> vector_frame(size_t index) { return m_plugin->vector_frame(index); }

private:
    explicit ImageDecoder(NonnullOwnPtr<ImageDecoderPlugin>);

    NonnullOwnPtr<ImageDecoderPlugin> mutable m_plugin;
};

}
