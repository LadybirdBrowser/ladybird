/*
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>

namespace JS {

using ImportedModuleReferrer = Variant<NonnullGCPtr<Script>, NonnullGCPtr<CyclicModule>, NonnullGCPtr<Realm>>;
using ImportedModulePayload = Variant<NonnullGCPtr<GraphLoadingState>, NonnullGCPtr<PromiseCapability>>;

}
