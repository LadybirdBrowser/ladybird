/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace Web::HTML {

// The settings of an environment another process hosts, as that process serialized them.
struct RemoteEnvironmentSettings final : public AtomicRefCounted<RemoteEnvironmentSettings> {
    static NonnullRefPtr<RemoteEnvironmentSettings const> create(SerializedEnvironmentSettingsObject settings)
    {
        return adopt_ref(*new RemoteEnvironmentSettings(move(settings)));
    }

    SerializedEnvironmentSettingsObject const settings;

private:
    explicit RemoteEnvironmentSettings(SerializedEnvironmentSettingsObject settings)
        : settings(move(settings))
    {
    }
};

}
