/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWakeLock/DisplaySleepInhibitor.h>

#include <AK/StdLibExtras.h>

namespace WakeLock {

namespace Detail {

struct DisplaySleepInhibitorImpl {
};

}

ErrorOr<DisplaySleepInhibitor> DisplaySleepInhibitor::create(StringView)
{
    return DisplaySleepInhibitor { make<Detail::DisplaySleepInhibitorImpl>() };
}

DisplaySleepInhibitor::DisplaySleepInhibitor(NonnullOwnPtr<Detail::DisplaySleepInhibitorImpl> impl)
    : m_impl(move(impl))
{
}

DisplaySleepInhibitor::DisplaySleepInhibitor(DisplaySleepInhibitor&&) = default;

DisplaySleepInhibitor& DisplaySleepInhibitor::operator=(DisplaySleepInhibitor&&) = default;

DisplaySleepInhibitor::~DisplaySleepInhibitor() = default;

}
