/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackStates/Forward.h>

namespace Media {

class SuspendedStateHandler final : public PlaybackStateHandler {
public:
    SuspendedStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    virtual ~SuspendedStateHandler() override = default;

    virtual void on_enter() override { }
    virtual void on_exit() override { }

    virtual void play() override;
    virtual void pause() override { }

    virtual bool is_playing() override
    {
        return false;
    }
    virtual PlaybackState state() override
    {
        return PlaybackState::Suspended;
    }
    virtual AvailableData available_data() override
    {
        return AvailableData::Future;
    }

    virtual void on_pipeline_status_changed(PipelineStatus) override;
};

}
