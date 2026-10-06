/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>

namespace JS {

using namespace EmbeddingABI;

Vector<Utf16FlyString> DeclarativeEnvironment::bindings() const
{
    Vector<Utf16FlyString> names;
    JSStringSink names_sink {
        .context = &names,
        .append = [](void* context, u16 const* code_units, size_t length_in_code_units) {
            auto name = utf16_view_from_abi({ code_units, length_in_code_units, false });
            static_cast<Vector<Utf16FlyString>*>(context)->append(Utf16FlyString::from_utf16(name));
        },
    };
    js_environment_declarative_binding_names(cell_to_abi<JSEnvironment>(*this), &names_sink);
    return names;
}

bool DeclarativeEnvironment::binding_is_mutable_by_name(Utf16FlyString const& name) const
{
    return js_environment_declarative_binding_is_mutable(cell_to_abi<JSEnvironment>(*this), utf16_view_to_abi(name.view()));
}

}
