/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/Types.h>

namespace Media {

enum class PipelineStatus : u8 {
    Pending,
    HaveData,
    Blocked,
    EndOfStream,
    Error,
};

constexpr bool is_waiting_for_data(PipelineStatus status)
{
    if (status == PipelineStatus::Pending)
        return true;
    if (status == PipelineStatus::Blocked)
        return true;
    return false;
}

constexpr bool can_carry_data(PipelineStatus status)
{
    if (status == PipelineStatus::HaveData)
        return true;
    if (status == PipelineStatus::EndOfStream)
        return true;
    return false;
}

constexpr PipelineStatus select_combined_pipeline_status(PipelineStatus a, PipelineStatus b)
{
    if (a == PipelineStatus::Error || b == PipelineStatus::Error)
        return PipelineStatus::Error;
    if (a == PipelineStatus::Blocked || b == PipelineStatus::Blocked)
        return PipelineStatus::Blocked;
    if (a == PipelineStatus::HaveData || b == PipelineStatus::HaveData)
        return PipelineStatus::HaveData;
    if (a == PipelineStatus::Pending || b == PipelineStatus::Pending)
        return PipelineStatus::Pending;
    return PipelineStatus::EndOfStream;
}

using PipelineStateChangeHandler = Function<void(PipelineStatus)>;

constexpr StringView pipeline_status_to_string(PipelineStatus status)
{
    switch (status) {
    case PipelineStatus::Pending:
        return "Pending"sv;
    case PipelineStatus::HaveData:
        return "HaveData"sv;
    case PipelineStatus::Blocked:
        return "Blocked"sv;
    case PipelineStatus::EndOfStream:
        return "EndOfStream"sv;
    case PipelineStatus::Error:
        return "Error"sv;
    }
    return "Invalid"sv;
}

}

namespace AK {

template<>
struct Formatter<Media::PipelineStatus> final : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::PipelineStatus state)
    {
        return Formatter<StringView>::format(builder, Media::pipeline_status_to_string(state));
    }
};

}
