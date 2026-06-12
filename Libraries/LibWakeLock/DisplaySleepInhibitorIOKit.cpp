/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWakeLock/DisplaySleepInhibitor.h>

#include <AK/StdLibExtras.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

namespace WakeLock {

namespace Detail {

struct DisplaySleepInhibitorImpl {
    explicit DisplaySleepInhibitorImpl(IOPMAssertionID assertion_id)
        : assertion_id(assertion_id)
    {
    }

    ~DisplaySleepInhibitorImpl()
    {
        IOPMAssertionRelease(assertion_id);
    }

    IOPMAssertionID assertion_id { 0 };
};

}

ErrorOr<DisplaySleepInhibitor> DisplaySleepInhibitor::create(StringView reason)
{
    auto const* reason_string = CFStringCreateWithBytes(nullptr, reinterpret_cast<u8 const*>(reason.characters_without_null_termination()), reason.length(), kCFStringEncodingUTF8, false);
    if (!reason_string)
        return Error::from_string_literal("Failed to create display sleep inhibition reason");

    IOPMAssertionID assertion_id = 0;
    auto result = IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, reason_string, &assertion_id);
    CFRelease(reason_string);
    if (result != kIOReturnSuccess)
        return Error::from_string_literal("Failed to inhibit display sleep");

    return DisplaySleepInhibitor { make<Detail::DisplaySleepInhibitorImpl>(assertion_id) };
}

DisplaySleepInhibitor::DisplaySleepInhibitor(NonnullOwnPtr<Detail::DisplaySleepInhibitorImpl> impl)
    : m_impl(move(impl))
{
}

DisplaySleepInhibitor::DisplaySleepInhibitor(DisplaySleepInhibitor&&) = default;

DisplaySleepInhibitor& DisplaySleepInhibitor::operator=(DisplaySleepInhibitor&&) = default;

DisplaySleepInhibitor::~DisplaySleepInhibitor() = default;

}
