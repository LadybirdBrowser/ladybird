/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

class BrowsingContext;
class Window;

WEB_API NonnullOwnPtr<JS::ExecutionContext> create_window_realm(GC::Ptr<Window>&, BrowsingContext&);

}
