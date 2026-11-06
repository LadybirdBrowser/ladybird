/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibJS/ScriptAndModuleABIConversions.h>

namespace JS {

using namespace EmbeddingABI;

// 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
Result<GC::Ref<Script>, Vector<ParserError>> Script::parse(Utf16View source_text, Realm& realm, StringView filename, Utf16View display_filename, GC::Ptr<GC::Cell> host_defined, size_t line_number_offset)
{
    ParserErrorCollector errors;
    auto utf16_filename = filename_to_utf16(filename);
    auto* script = js_script_parse(
        vm_to_abi(realm.vm()),
        realm_to_abi(realm),
        utf16_view_to_abi(source_text),
        utf16_view_to_abi(utf16_filename.utf16_view()),
        utf16_view_to_abi(display_filename),
        host_defined.ptr(),
        line_number_offset,
        errors.sink());
    return record_or_parser_errors_from_abi<Script>(script, errors);
}

GC::Ptr<GC::Cell> Script::host_defined() const
{
    return static_cast<GC::Cell*>(js_script_host_defined(script_to_abi(*this)));
}

}
