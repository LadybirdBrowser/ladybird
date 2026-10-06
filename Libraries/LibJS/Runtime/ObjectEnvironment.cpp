/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ObjectEnvironment.h>

namespace JS {

using namespace EmbeddingABI;

Object& ObjectEnvironment::binding_object()
{
    return object_from_abi(js_environment_object_binding_object(cell_to_abi<JSEnvironment>(*this)));
}

}
