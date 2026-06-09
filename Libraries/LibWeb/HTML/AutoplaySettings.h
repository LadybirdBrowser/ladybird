/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/AutoplayPolicy.h>

namespace Web::HTML {

enum class AutoplayDecision : u8 {
    Allowed,
    AllowedIfInaudible,
    Blocked,
};

// Holds the user agent's autoplay configuration for this process: the global policy plus the set of
// origins that may always autoplay regardless of that policy. Populated from the browser process.
class WEB_API AutoplaySettings {
public:
    static AutoplaySettings& the();

    AutoplayDecision decision_for_origin(DOM::Document const&, URL::Origin const&) const;

    void set_policy(AutoplayPolicy, ReadonlySpan<String> allowlist);

private:
    AutoplaySettings();
    ~AutoplaySettings();

    AutoplayPolicy m_policy { AutoplayPolicy::BlockAudio };
    Vector<URL::Origin> m_allowlist;
};

}
