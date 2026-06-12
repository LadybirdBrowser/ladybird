/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>

namespace WakeLock {

namespace Detail {

struct DisplaySleepInhibitorImpl;

}

class DisplaySleepInhibitor {
public:
    static ErrorOr<DisplaySleepInhibitor> create(StringView reason);

    DisplaySleepInhibitor(DisplaySleepInhibitor const&) = delete;
    DisplaySleepInhibitor& operator=(DisplaySleepInhibitor const&) = delete;

    DisplaySleepInhibitor(DisplaySleepInhibitor&&);
    DisplaySleepInhibitor& operator=(DisplaySleepInhibitor&&);

    ~DisplaySleepInhibitor();

private:
    explicit DisplaySleepInhibitor(NonnullOwnPtr<Detail::DisplaySleepInhibitorImpl>);

    OwnPtr<Detail::DisplaySleepInhibitorImpl> m_impl;
};

}
