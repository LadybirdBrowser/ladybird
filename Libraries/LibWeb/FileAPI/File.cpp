/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/FileAPI/File.h>

namespace Web::FileAPI {

extern "C" {

// Defined in Bindings/Rust/src/file_api/exports.rs.
void* web_file_create(JSVM*, ReadonlyBytes, FlatPtr file_name, FlatPtr type, bool has_last_modified, i64 last_modified);
FlatPtr web_file_name(void* file);
i64 web_file_last_modified(void* file);
}

GC::Ref<File> File::create(ReadonlyBytes bytes, Utf16String file_name, Utf16String type, Optional<i64> last_modified)
{
    auto* file = web_file_create(JS::HostABI::vm_to_abi(JS::VM::the()), bytes, move(file_name).into_raw(), move(type).into_raw(), last_modified.has_value(), last_modified.value_or(0));
    return GC::Ref { *static_cast<File*>(file) };
}

Utf16String File::name() const
{
    return Utf16String::from_raw(web_file_name(const_cast<File*>(this)));
}

i64 File::last_modified() const
{
    return web_file_last_modified(const_cast<File*>(this));
}

}
