/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Providers/MediaTimeProvider.h>

namespace Media {

class GenericTimeProvider final : public MediaTimeProvider {
public:
    GenericTimeProvider();
    virtual ~GenericTimeProvider() override;

    virtual AK::Duration current_time() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void set_time(AK::Duration) override;

private:
    Optional<MonotonicTime> m_monotonic_time_on_resume;
    AK::Duration m_media_time;
};

}
