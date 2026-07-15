/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCore/Process.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API FirefoxClient {
public:
    static NonnullOwnPtr<FirefoxClient> create();
    ~FirefoxClient();

    ErrorOr<void> ensure_running(u16 port, u64 tab_id);
    bool is_running() const;

private:
    FirefoxClient() = default;

    Optional<Core::Process> m_process;
};

}
