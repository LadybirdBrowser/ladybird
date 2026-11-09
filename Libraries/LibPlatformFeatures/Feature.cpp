/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPlatformFeatures/Feature.h>

namespace PlatformFeatures {

StringView feature_id(Feature feature)
{
    switch (feature) {
    case Feature::GpuPainting:
        return "gpu_painting"sv;
    case Feature::YCbCrImport:
        return "ycbcr_import"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Feature> feature_from_id(StringView id)
{
    for (auto feature : all_features) {
        if (feature_id(feature) == id)
            return feature;
    }
    return {};
}

FeatureProcess process_of(Feature feature)
{
    switch (feature) {
    case Feature::GpuPainting:
    case Feature::YCbCrImport:
        return FeatureProcess::Compositor;
    }
    VERIFY_NOT_REACHED();
}

Optional<Feature> dependency_of(Feature feature)
{
    switch (feature) {
    case Feature::GpuPainting:
        return {};
    case Feature::YCbCrImport:
        return Feature::GpuPainting;
    }
    VERIFY_NOT_REACHED();
}

StringView process_name(FeatureProcess process)
{
    switch (process) {
    case FeatureProcess::Compositor:
        return "Compositor"sv;
    }
    VERIFY_NOT_REACHED();
}

}
