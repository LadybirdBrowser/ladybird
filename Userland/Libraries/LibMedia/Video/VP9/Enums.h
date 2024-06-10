/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Symbols.h"
#include <AK/Types.h>

namespace Media::Video::VP9 {

enum class FrameType {
    KeyFrame,
    IntraOnlyFrame,
    InterFrame
};

enum ColorSpace : u8 {
    Unknown = 0,
    Bt601 = 1,
    Bt709 = 2,
    Smpte170 = 3,
    Smpte240 = 4,
    Bt2020 = 5,
    Reserved = 6,
    RGB = 7
};

enum InterpolationFilter : u8 {
    EightTap = 0,
    EightTapSmooth = 1,
    EightTapSharp = 2,
    Bilinear = 3,
    Switchable = 4
};

enum ReferenceFrameType : u8 {
    // None represents both INTRA_FRAME and NONE in the spec. When the primary reference
    // frame type is None, that means that the frame/block is not inter-predicted.
    None = 0,
    LastFrame = 1,
    GoldenFrame = 2,
    AltRefFrame = 3,
};

enum class TransformMode : u8 {
    Only_4x4 = 0,
    Allow_8x8 = 1,
    Allow_16x16 = 2,
    Allow_32x32 = 3,
    Select = 4,
};

enum TransformSize : u8 {
    Transform_4x4 = 0,
    Transform_8x8 = 1,
    Transform_16x16 = 2,
    Transform_32x32 = 3,
};

enum class TransformType : u8 {
    DCT = 0,
    ADST = 1,
};

struct TransformSet {
    TransformType first_transform : 1;
    TransformType second_transform : 1;

    bool operator==(TransformSet const& other) const
    {
        return first_transform == other.first_transform && second_transform == other.second_transform;
    }
};

enum ReferenceMode : u8 {
    SingleReference = 0,
    CompoundReference = 1,
    ReferenceModeSelect = 2,
};

enum class ReferenceIndex : u8 {
    Primary = 0,
    Secondary = 1,
};

enum BlockSubsize : u8 {
    Block_4x4 = 0,
    Block_4x8 = 1,
    Block_8x4 = 2,
    Block_8x8 = 3,
    Block_8x16 = 4,
    Block_16x8 = 5,
    Block_16x16 = 6,
    Block_16x32 = 7,
    Block_32x16 = 8,
    Block_32x32 = 9,
    Block_32x64 = 10,
    Block_64x32 = 11,
    Block_64x64 = 12,
    Block_Invalid = BLOCK_INVALID
};

enum Partition : u8 {
    PartitionNone = 0,
    PartitionHorizontal = 1,
    PartitionVertical = 2,
    PartitionSplit = 3,
};

enum class PredictionMode : u8 {
    DcPred = 0,
    VPred = 1,
    HPred = 2,
    D45Pred = 3,
    D135Pred = 4,
    D117Pred = 5,
    D153Pred = 6,
    D207Pred = 7,
    D63Pred = 8,
    TmPred = 9,
    NearestMv = 10,
    NearMv = 11,
    ZeroMv = 12,
    NewMv = 13,
};

enum MvJoint : u8 {
    MotionVectorAllZero = 0,
    MotionVectorNonZeroColumn = 1,
    MotionVectorNonZeroRow = 2,
};

enum MvClass : u8 {
    MvClass0 = 0,
    MvClass1 = 1,
    MvClass2 = 2,
    MvClass3 = 3,
    MvClass4 = 4,
    MvClass5 = 5,
    MvClass6 = 6,
    MvClass7 = 7,
    MvClass8 = 8,
    MvClass9 = 9,
    MvClass10 = 10,
};

enum Token : u8 {
    ZeroToken = 0,
    OneToken = 1,
    TwoToken = 2,
    ThreeToken = 3,
    FourToken = 4,
    DctValCat1 = 5,
    DctValCat2 = 6,
    DctValCat3 = 7,
    DctValCat4 = 8,
    DctValCat5 = 9,
    DctValCat6 = 10,
};

enum class SegmentFeature : u8 {
    // SEG_LVL_ALT_Q
    AlternativeQuantizerBase,
    // SEG_LVL_ALT_L
    AlternativeLoopFilterBase,
    // SEG_LVL_REF_FRAME
    ReferenceFrameOverride,
    // SEG_LVL_SKIP
    SkipResidualsOverride,
    // SEG_LVL_MAX
    Sentinel,
};

}
