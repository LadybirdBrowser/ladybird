/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Types.h>

namespace Core {

// Strongly-typed category for both markers and labels. Adding a value here
// will break exhaustive switches in GeckoProfileWriter's build_categories()
// and category_to_underlying(), forcing the update to ripple out.
enum class MarkerCategory : u8 {
    Other = 0,
    Idle = 1,
    Layout = 2,
    JavaScript = 3,
    GC = 4,
    Network = 5,
    Graphics = 6,
    DOM = 7,
    IPC = 8,
    Media = 9,
    Timer = 10,
    Profiler = 11,
    Accessibility = 12,
    Style = 13,
    Paint = 14,
    Parser = 15,
};

ALWAYS_INLINE StringView marker_category_name(MarkerCategory category)
{
    switch (category) {
    case MarkerCategory::Other:
        return "Other"sv;
    case MarkerCategory::Idle:
        return "Idle"sv;
    case MarkerCategory::Layout:
        return "Layout"sv;
    case MarkerCategory::JavaScript:
        return "JavaScript"sv;
    case MarkerCategory::GC:
        return "GC"sv;
    case MarkerCategory::Network:
        return "Network"sv;
    case MarkerCategory::Graphics:
        return "Graphics"sv;
    case MarkerCategory::DOM:
        return "DOM"sv;
    case MarkerCategory::IPC:
        return "IPC"sv;
    case MarkerCategory::Media:
        return "Media"sv;
    case MarkerCategory::Timer:
        return "Timer"sv;
    case MarkerCategory::Profiler:
        return "Profiler"sv;
    case MarkerCategory::Accessibility:
        return "Accessibility"sv;
    case MarkerCategory::Style:
        return "Style"sv;
    case MarkerCategory::Paint:
        return "Paint"sv;
    case MarkerCategory::Parser:
        return "Parser"sv;
    }
    VERIFY_NOT_REACHED();
}

}
