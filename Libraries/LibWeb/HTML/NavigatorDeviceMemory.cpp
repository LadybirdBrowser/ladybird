/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <LibCore/System.h>
#include <LibWeb/HTML/NavigatorDeviceMemory.h>

namespace Web::HTML {

// https://www.w3.org/TR/device-memory/#computing-device-memory-value
WebIDL::Double NavigatorDeviceMemoryMixin::device_memory() const
{
    // The value is calculated by using the actual device memory in MiB then rounding it to the nearest number where
    // only the most significant bit can be set and the rest are zeros (nearest power of two).
    auto memory_in_bytes = Core::System::physical_memory_bytes();
    auto memory_in_mib = memory_in_bytes / MiB;
    auto required_bits = AK::count_required_bits(memory_in_mib);
    auto lower_memory_in_mib = static_cast<u64>(1) << (required_bits - 1);
    auto upper_memory_in_mib = static_cast<u64>(1) << required_bits;
    auto rounded_memory_in_mib = upper_memory_in_mib - memory_in_mib <= memory_in_mib - lower_memory_in_mib
        ? upper_memory_in_mib
        : lower_memory_in_mib;

    // Then dividing that number by 1024.0 to get the value in GiB.
    auto memory_in_gib = static_cast<WebIDL::Double>(rounded_memory_in_mib) / 1024.0;

    // An upper bound and a lower bound should be set on the list of values.
    return AK::clamp(memory_in_gib, 1.0, 4.0);
}

}
