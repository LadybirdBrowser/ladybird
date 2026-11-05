/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibWebView/Export.h>
#include <LibWebView/LingeringIndicator.h>

namespace WebView {

enum class TabIndicator : u8 {
    PlayingAudio,
    UsingLocation,
    UsingMicrophone,
};

// Each indicator lingers on a deadline of its own once what it shows stops, however it stopped.
class WEBVIEW_API TabIndicators {
    AK_MAKE_NONCOPYABLE(TabIndicators);
    AK_MAKE_NONMOVABLE(TabIndicators);

public:
    static constexpr AK::Duration LINGER = AK::Duration::from_seconds(3);

    TabIndicators();
    ~TabIndicators();

    Function<void(TabIndicator, bool visible)> on_visibility_change;

    void set_active(TabIndicator, bool active, MonotonicTime now);
    bool is_active(TabIndicator) const;
    bool is_visible(TabIndicator indicator) const { return entry(indicator).visible; }

    void update_visibility(MonotonicTime now);
    Optional<MonotonicTime> next_deadline(MonotonicTime now) const;

private:
    struct Entry {
        TabIndicator indicator;
        LingeringIndicator lingering { LINGER };
        bool visible { false };
        bool reported_visible { false };
    };

    Entry& entry(TabIndicator);
    Entry const& entry(TabIndicator indicator) const { return const_cast<TabIndicators&>(*this).entry(indicator); }

    Array<Entry, 3> m_entries {
        Entry { .indicator = TabIndicator::PlayingAudio },
        Entry { .indicator = TabIndicator::UsingLocation },
        Entry { .indicator = TabIndicator::UsingMicrophone },
    };
    NonnullRefPtr<Core::Timer> m_timer;
};

}
