/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/WebDriver/UserPrompt.h>

namespace Web::WebDriver {

WEB_API void handle_any_user_prompts(Page&, GC::Ref<GC::Function<void(Optional<Error>)>> on_complete);

}
