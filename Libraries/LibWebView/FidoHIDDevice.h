/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Error.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebView/Forward.h>

namespace WebView {

// A USB HID device with the FIDO usage page, which is how a security key shows itself to the host,
// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-20210615.html
// A device belongs to the thread that opened it.
class FidoHIDDevice {
public:
    AK_ALLOC_WITH_KMALLOC;

    // NB: Full speed devices, which security keys are, have reports of 64 bytes.
    static constexpr size_t report_size = 64;
    using Report = Array<u8, report_size>;

    virtual ~FidoHIDDevice() = default;

    // What tells the device from the others for as long as it stays connected.
    virtual u64 id() const = 0;

    // Sends one output report of report_size bytes.
    virtual ErrorOr<void> write_report(ReadonlyBytes) = 0;
    // The next input report, or nothing when none came in the time. Waiting also takes in the reports of the other
    // devices of the thread.
    virtual ErrorOr<Optional<Report>> read_report(u32 timeout_in_milliseconds) = 0;

    // Opens the connected devices that are not among the known ones.
    static Vector<NonnullOwnPtr<FidoHIDDevice>> open_new_devices(HashTable<u64> const& known_device_ids);
    static bool is_any_connected();
};

}
