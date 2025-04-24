/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Scripting/Agent.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#dedicated-worker-agent
// https://html.spec.whatwg.org/multipage/webappapis.html#shared-worker-agent
struct WorkerAgent : public Agent {
    static NonnullOwnPtr<WorkerAgent> create(GC::Heap&, CanBlock);

private:
    using Agent::Agent;
};

}
