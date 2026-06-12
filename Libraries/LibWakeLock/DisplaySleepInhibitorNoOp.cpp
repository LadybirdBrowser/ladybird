/*
 * Copyright (c) 2026, the Ladybird developers.
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

DisplaySleepInhibitor::DisplaySleepInhibitor(DisplaySleepInhibitor&& other)
    : m_impl(move(other.m_impl))
{
}

DisplaySleepInhibitor& DisplaySleepInhibitor::operator=(DisplaySleepInhibitor&& other)
{
    if (this != &other)
        m_impl = move(other.m_impl);
    return *this;
}

DisplaySleepInhibitor::~DisplaySleepInhibitor() = default;

}
