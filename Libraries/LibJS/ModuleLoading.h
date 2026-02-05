/*
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>

namespace JS {

using ImportedModuleReferrer = Variant<GC::Ref<Script>, GC::Ref<CyclicModule>, GC::Ref<Realm>>;
using ImportedModulePayload = Variant<GC::Ref<GraphLoadingState>, GC::Ref<PromiseCapability>>;

}
