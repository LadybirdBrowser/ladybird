/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackStates/ResumingStateHandler.h>

namespace Media {

class StartingStateHandler final : public ResumingStateHandler {
public:
    StartingStateHandler(PlaybackManager& manager)
        : ResumingStateHandler(manager, false)
    {
    }
    virtual ~StartingStateHandler() override = default;

    virtual void start() override;

    virtual PlaybackState state() override
    {
        return PlaybackState::Starting;
    }
    virtual AvailableData available_data() override
    {
        return AvailableData::None;
    }

    virtual void on_pipeline_status_changed(PipelineStatus) override;

private:
    bool m_started { false };
    bool m_pipeline_blocked { false };
};

}
