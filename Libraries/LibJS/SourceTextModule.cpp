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

Result<GC::Ref<SourceTextModule>, Vector<ParserError>> SourceTextModule::parse_from_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache> bytecode_cache, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    ParserErrorCollector errors;
    auto utf16_filename = filename_to_utf16(filename);
    auto* module = js_bytecode_cache_create_module(
        vm_to_abi(realm.vm()),
        decoded_bytecode_cache_to_abi(*bytecode_cache),
        source_code_to_abi(*source_code),
        realm_to_abi(realm),
        utf16_view_to_abi(utf16_filename.utf16_view()),
        host_defined.ptr(),
        errors.sink());
    return record_or_parser_errors_from_abi<SourceTextModule>(module, errors);
}

void SourceTextModule::begin_bytecode_cache_generation()
{
    js_bytecode_cache_module_begin_generation(vm_to_abi(vm()), module_to_abi(*this));
}

void SourceTextModule::finish_bytecode_cache_generation_without_install()
{
    js_bytecode_cache_module_finish_generation_without_install(vm_to_abi(vm()), module_to_abi(*this));
}

void SourceTextModule::install_generated_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache> bytecode_cache, NonnullRefPtr<SourceCode const> source_code)
{
    js_bytecode_cache_module_install_generated(vm_to_abi(vm()), module_to_abi(*this), decoded_bytecode_cache_to_abi(*bytecode_cache), source_code_to_abi(*source_code));
}

}
