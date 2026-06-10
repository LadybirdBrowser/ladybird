/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/WorkletGlobalScope.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkletGlobalScope);

WorkletGlobalScope::WorkletGlobalScope()
{
}

WorkletGlobalScope::~WorkletGlobalScope() = default;

}
