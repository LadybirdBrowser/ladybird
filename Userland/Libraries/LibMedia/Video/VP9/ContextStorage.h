/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

#include "Enums.h"
#include "LookupTables.h"
#include "MotionVector.h"

namespace Media::Video::VP9 {

template<typename T>
struct ReferencePair {
    T primary;
    T secondary;

    T& operator[](ReferenceIndex index)
    {
        switch (index) {
        case ReferenceIndex::Primary:
            return primary;
        case ReferenceIndex::Secondary:
            return secondary;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    T const& operator[](ReferenceIndex index) const
    {
        return const_cast<ReferencePair<T>&>(*this)[index];
    }
};

typedef ReferencePair<ReferenceFrameType> ReferenceFramePair;
typedef ReferencePair<MotionVector> MotionVectorPair;

template<typename T>
class Vector2D;

template<typename T>
class Vector2DView {
public:
    u32 top() const { return m_top; }
    u32 left() const { return m_left; }
    u32 height() const { return m_height; }
    u32 width() const { return m_width; }

    T const& operator[](size_t index) const { return m_storage[index]; }
    size_t size() const { return m_storage->size(); }

    T& at(u32 relative_row, u32 relative_column)
    {
        VERIFY(relative_row < height());
        VERIFY(relative_column < width());
        return m_storage->at(top() + relative_row, left() + relative_column);
    }
    T const& at(u32 relative_row, u32 relative_column) const
    {
        VERIFY(relative_row < height());
        VERIFY(relative_column < width());
        return m_storage->at(top() + relative_row, left() + relative_column);
    }

    Vector2DView<T> view(u32 top, u32 left, u32 height, u32 width)
    {
        VERIFY(top + height <= this->height());
        VERIFY(left + width <= this->width());
        return Vector2DView<T>(m_storage, this->top() + top, this->left() + left, height, width);
    }

private:
    friend class Vector2D<T>;

    Vector2DView(Vector2D<T>* const storage, u32 top, u32 left, u32 height, u32 width)
        : m_storage(storage)
        , m_top(top)
        , m_left(left)
        , m_height(height)
        , m_width(width)
    {
    }

    Vector2D<T>* const m_storage;
    u32 const m_top { 0 };
    u32 const m_left { 0 };
    u32 const m_height { 0 };
    u32 const m_width { 0 };
};

template<typename T>
class Vector2D {
public:
    ~Vector2D()
    {
        clear_storage();
    }

    ErrorOr<void> try_resize(u32 height, u32 width)
    {
        if (height != m_height && width != m_width) {
            clear_storage();

            size_t size = height * width;
            auto* new_storage = new (nothrow) T[size];
            if (!new_storage)
                return Error::from_errno(ENOMEM);
            m_storage = new_storage;
            m_height = height;
            m_width = width;
        }

        return {};
    }

    u32 height() const { return m_height; }
    u32 width() const { return m_width; }

    size_t index_at(u32 row, u32 column) const
    {
        VERIFY(row < height());
        VERIFY(column < width());
        return row * width() + column;
    }

    T& operator[](size_t index) { return m_storage[index]; }
    T const& operator[](size_t index) const { return m_storage[index]; }
    size_t size() const { return m_height * m_width; }

    T& at(u32 row, u32 column)
    {
        return m_storage[index_at(row, column)];
    }
    T const& at(u32 row, u32 column) const
    {
        return m_storage[index_at(row, column)];
    }

    void assign(u32 row, u32 column, T&& value)
    {
        new (&m_storage[index_at(row, column)]) T(move(value));
    }

    template<typename OtherT, typename Function>
    void copy_to(Vector2D<OtherT>& other, Function function) const
    {
        VERIFY(width() <= other.width());
        VERIFY(height() <= other.height());
        for (u32 row = 0; row < height(); row++) {
            for (u32 column = 0; column < width(); column++)
                other.at(row, column) = function(at(row, column));
        }
    }

    void copy_to(Vector2D<T>& other) const
    {
        VERIFY(width() <= other.width());
        VERIFY(height() <= other.height());
        for (u32 row = 0; row < height(); row++) {
            auto other_index = other.index_at(row, 0);
            AK::TypedTransfer<T>::copy(&m_storage[index_at(row, 0)], &other[other_index], width());
        }
    }

    template<typename OtherT>
    ErrorOr<void> try_resize_to_match_other_vector2d(Vector2D<OtherT> const& other)
    {
        return try_resize(other.height(), other.width());
    }

    void reset()
    {
        for (size_t i = 0; i < size(); i++)
            m_storage[i] = T();
    }

    Vector2DView<T> view(u32 top, u32 left, u32 height, u32 width)
    {
        VERIFY(top + height <= this->height());
        VERIFY(left + width <= this->width());
        return Vector2DView<T>(this, top, left, height, width);
    }

private:
    void clear_storage()
    {
        delete[] m_storage;
        m_storage = nullptr;
        m_width = 0;
        m_height = 0;
    }

    u32 m_height { 0 };
    u32 m_width { 0 };
    T* m_storage { nullptr };
};

// Block context that is kept for the lifetime of a frame.
struct FrameBlockContext {
    bool is_intra_predicted() const { return ref_frames.primary == ReferenceFrameType::None; }
    bool is_single_reference() const { return ref_frames.secondary == ReferenceFrameType::None; }
    MotionVectorPair primary_motion_vector_pair() const { return sub_block_motion_vectors[3]; }

    bool is_available { false };
    bool skip_coefficients { false };
    TransformSize transform_size { Transform_4x4 };
    PredictionMode y_mode { PredictionMode::DcPred };
    Array<PredictionMode, 4> sub_modes { PredictionMode::DcPred, PredictionMode::DcPred, PredictionMode::DcPred, PredictionMode::DcPred };
    InterpolationFilter interpolation_filter { InterpolationFilter::EightTap };
    ReferenceFramePair ref_frames { ReferenceFrameType::None, ReferenceFrameType::None };
    Array<MotionVectorPair, 4> sub_block_motion_vectors;
    u8 segment_id { 0 };
};

// Block context that is kept between frames until explicitly cleared.
struct PersistentBlockContext {
    PersistentBlockContext()
        : available(false)
    {
    }

    PersistentBlockContext(FrameBlockContext const& frame_context)
        : available(frame_context.is_available)
        , ref_frames(frame_context.ref_frames)
        , primary_motion_vector_pair(frame_context.primary_motion_vector_pair())
        , segment_id(frame_context.segment_id)
    {
    }

    bool available { false };
    ReferenceFramePair ref_frames { ReferenceFrameType::None, ReferenceFrameType::None };
    MotionVectorPair primary_motion_vector_pair {};
    u8 segment_id { 0 };
};

struct SegmentFeatureStatus {
    bool enabled { false };
    u8 value { 0 };
};

using SegmentFeatures = Array<SegmentFeatureStatus, to_underlying(SegmentFeature::Sentinel)>;
using SegmentationFeatures = Array<SegmentFeatures, MAX_SEGMENTS>;

struct ColorConfig {
    u8 bit_depth { 8 };
    ColorSpace color_space { ColorSpace::Bt601 };
    VideoFullRangeFlag color_range { VideoFullRangeFlag::Studio };
    bool subsampling_x { true };
    bool subsampling_y { true };
};

struct BlockMotionVectorCandidateSet;
using BlockMotionVectorCandidates = ReferencePair<BlockMotionVectorCandidateSet>;

using NonZeroTokens = Array<FixedArray<bool>, 3>;
using NonZeroTokensView = Array<Span<bool>, 3>;

using SegmentationPredictionContext = FixedArray<u8>;
using SegmentationPredictionContextView = Span<u8>;

using PartitionContext = FixedArray<u8>;
using PartitionContextView = Span<u8>;

struct ReferenceFrame {
    Gfx::Size<u32> size { 0, 0 };
    bool subsampling_x { false };
    bool subsampling_y { false };
    u8 bit_depth { 0 };
    Array<Vector<u16>, 3> frame_planes {};

    bool is_valid() const { return bit_depth > 0; }

    // These values are set at the start of each inter frame to be used during prediction.
    i32 x_scale { 0 };
    i32 y_scale { 0 };
    i32 scaled_step_x { 0 };
    i32 scaled_step_y { 0 };
};

}
