/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>

namespace JS {

using namespace EmbeddingABI;

// 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
Result<GC::Ref<Script>, Vector<ParserError>> Script::parse(Utf16View source_text, Realm& realm, StringView filename, Utf16View display_filename, GC::Ptr<GC::Cell> host_defined, size_t line_number_offset)
{
    Vector<ParserError> errors;
    JSParserErrorSink error_sink {
        .context = &errors,
        .append = [](void* context, JSOwnedUtf16String message, u32 line, u32 column) {
            static_cast<Vector<ParserError>*>(context)->append({ owned_utf16_string_from_abi(message), Position { line, column } });
        },
    };

    auto utf16_filename = Utf16String::from_utf8(filename);
    auto* script = js_script_parse(
        vm_to_abi(realm.vm()),
        cell_to_abi<JSRealm>(realm),
        utf16_view_to_abi(source_text),
        utf16_view_to_abi(utf16_filename),
        utf16_view_to_abi(display_filename),
        host_defined.ptr(),
        line_number_offset,
        &error_sink);
    if (!script) {
        VERIFY(!errors.is_empty());
        return errors;
    }
    return GC::Ref { *cell_from_abi<Script>(script) };
}

GC::Ptr<GC::Cell> Script::host_defined() const
{
    return static_cast<GC::Cell*>(js_script_host_defined(cell_to_abi<JSScript>(*this)));
}

}
