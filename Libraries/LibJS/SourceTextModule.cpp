/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibJS/ScriptAndModuleABIConversions.h>
#include <LibJS/SourceTextModule.h>

namespace JS {

using namespace EmbeddingABI;

// 16.2.1.6.1 ParseModule ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parsemodule
Result<GC::Ref<SourceTextModule>, Vector<ParserError>> SourceTextModule::parse(Utf16View source_text, Realm& realm, StringView filename, Utf16View display_filename, GC::Ptr<GC::Cell> host_defined, size_t line_number_offset)
{
    ParserErrorCollector errors;
    auto utf16_filename = filename_to_utf16(filename);
    auto* module = js_module_parse_source_text_module(
        vm_to_abi(realm.vm()),
        realm_to_abi(realm),
        utf16_view_to_abi(source_text),
        utf16_view_to_abi(utf16_filename.utf16_view()),
        utf16_view_to_abi(display_filename),
        host_defined.ptr(),
        line_number_offset,
        errors.sink());
    return record_or_parser_errors_from_abi<SourceTextModule>(module, errors);
}

}
