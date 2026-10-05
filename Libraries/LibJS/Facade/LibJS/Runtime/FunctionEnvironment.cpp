/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/FunctionEnvironment.h>

namespace JS {

using namespace EmbeddingABI;

FunctionObject& FunctionEnvironment::function_object()
{
    return *cell_from_abi<FunctionObject>(js_environment_function_object(cell_to_abi<JSEnvironment>(*this)));
}

FunctionObject const& FunctionEnvironment::function_object() const
{
    return *cell_from_abi<FunctionObject>(js_environment_function_object(cell_to_abi<JSEnvironment>(*this)));
}

}
