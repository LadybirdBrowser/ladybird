/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

[[nodiscard]] GC::Ref<Directive> create_directive(GC::Heap&, Utf16FlyString name, Vector<Utf16String> value);

}
