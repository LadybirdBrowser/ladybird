/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibPlatformFeatures/ProcessFeatures.h>

namespace PlatformFeatures {

static NeverDestroyed<Optional<ProcessFeatures>> s_current_process_features;

ErrorOr<ProcessFeatures> ProcessFeatures::from_string(FeatureProcess process, StringView string)
{
    ProcessFeatures features { process };
    for (auto id : string.split_view(',')) {
        auto feature = feature_from_id(id);
        if (!feature.has_value())
            return Error::from_string_literal("Unknown platform feature");
        if (process_of(*feature) != process)
            return Error::from_string_literal("A platform feature was given to a process that does not use it");
        features.set_enabled(*feature);
    }
    return features;
}

ByteString ProcessFeatures::to_byte_string() const
{
    StringBuilder builder;
    for (auto feature : all_features) {
        if (!m_enabled.contains(feature))
            continue;
        if (!builder.is_empty())
            builder.append(',');
        builder.append(feature_id(feature));
    }
    return builder.to_byte_string();
}

bool ProcessFeatures::is_enabled(Feature feature) const
{
    VERIFY(process_of(feature) == m_process);
    return m_enabled.contains(feature);
}

void ProcessFeatures::set_enabled(Feature feature)
{
    VERIFY(process_of(feature) == m_process);
    m_enabled.set(feature);
}

void ProcessFeatures::initialize_for_current_process(ProcessFeatures features)
{
    VERIFY(!s_current_process_features->has_value());
    *s_current_process_features = move(features);
}

ProcessFeatures const& ProcessFeatures::the()
{
    return s_current_process_features->value();
}

}
