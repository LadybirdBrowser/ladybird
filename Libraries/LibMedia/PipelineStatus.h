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
    MovedPosition,
    Blocked,
    EndOfStream,
    Error,
};

constexpr bool is_waiting_for_data(PipelineStatus status)
{
    return status != PipelineStatus::HaveData;
}

constexpr bool is_terminal(PipelineStatus status)
{
    if (status == PipelineStatus::Error)
        return true;
    if (status == PipelineStatus::EndOfStream)
        return true;
    return false;
}

constexpr bool resolves_seek(PipelineStatus status)
{
    if (!is_waiting_for_data(status))
        return true;
    if (is_terminal(status))
        return true;
    return false;
}

constexpr bool status_change_should_wake(PipelineStatus previous, PipelineStatus current)
{
    if (previous == current)
        return false;
    if (!resolves_seek(current))
        return false;
    if (is_waiting_for_data(previous))
        return true;
    if (previous == PipelineStatus::EndOfStream)
        return true;
    return false;
}

constexpr PipelineStatus select_combined_pipeline_status(PipelineStatus a, PipelineStatus b)
{
    if (a == PipelineStatus::Error || b == PipelineStatus::Error)
        return PipelineStatus::Error;
    if (a == PipelineStatus::Blocked || b == PipelineStatus::Blocked)
        return PipelineStatus::Blocked;
    if (a == PipelineStatus::Pending || b == PipelineStatus::Pending)
        return PipelineStatus::Pending;
    if (a == PipelineStatus::HaveData || b == PipelineStatus::HaveData)
        return PipelineStatus::HaveData;
    if (a == PipelineStatus::MovedPosition || b == PipelineStatus::MovedPosition)
        return PipelineStatus::MovedPosition;
    return PipelineStatus::EndOfStream;
}

using PipelineStateChangeHandler = Function<void(PipelineStatus)>;
using PipelineWakeHandler = Function<void()>;

constexpr StringView pipeline_status_to_string(PipelineStatus status)
{
    switch (status) {
    case PipelineStatus::Pending:
        return "Pending"sv;
    case PipelineStatus::HaveData:
        return "HaveData"sv;
    case PipelineStatus::MovedPosition:
        return "MovedPosition"sv;
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
