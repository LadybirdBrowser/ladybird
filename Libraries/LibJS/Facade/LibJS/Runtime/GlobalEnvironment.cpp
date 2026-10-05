/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>

namespace JS {

using namespace EmbeddingABI;

Object& GlobalEnvironment::global_this_value()
{
    return object_from_abi(js_environment_global_this_value(cell_to_abi<JSEnvironment>(*this)));
}

DeclarativeEnvironment& GlobalEnvironment::declarative_record()
{
    return *cell_from_abi<DeclarativeEnvironment>(js_environment_global_declarative_record(cell_to_abi<JSEnvironment>(*this)));
}

}
