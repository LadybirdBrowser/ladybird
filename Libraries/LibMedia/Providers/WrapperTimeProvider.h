/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Providers/MediaTimeProvider.h>

namespace Media {

template<typename T>
class WrapperTimeProvider final : public MediaTimeProvider {
public:
    WrapperTimeProvider(T& inner)
        : m_inner(inner)
    {
    }
    virtual ~WrapperTimeProvider() override = default;

    virtual AK::Duration current_time() const override { return m_inner->current_time(); }
    virtual void resume() override { m_inner->resume(); }
    virtual void pause() override { m_inner->pause(); }
    virtual void set_time(AK::Duration time) override { m_inner->set_time(time); }

private:
    NonnullRefPtr<T> m_inner;
};

}
