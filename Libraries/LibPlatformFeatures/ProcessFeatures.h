/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashTable.h>
#include <LibPlatformFeatures/Feature.h>

namespace PlatformFeatures {

// The features one process may use, decided by the Browser before launching it and fixed for the process's lifetime.
class ProcessFeatures {
public:
    explicit ProcessFeatures(FeatureProcess process)
        : m_process(process)
    {
    }

    // The text form names the enabled features, comma-separated, so that it can be passed as an argument.
    static ErrorOr<ProcessFeatures> from_string(FeatureProcess, StringView);
    ByteString to_byte_string() const;

    bool is_enabled(Feature) const;
    void set_enabled(Feature);

    static void initialize_for_current_process(ProcessFeatures);
    static ProcessFeatures const& the();

private:
    FeatureProcess m_process;
    HashTable<Feature> m_enabled;
};

}
