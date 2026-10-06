/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Result.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/DecodedBytecodeCache.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/SourceCode.h>

namespace JS {

// 16.2.1.6 Source Text Module Records, https://tc39.es/ecma262/#sec-source-text-module-records
class JS_API SourceTextModule final : public CyclicModule {
public:
    static Result<GC::Ref<SourceTextModule>, Vector<ParserError>> parse(Utf16View source_text, Realm&, StringView filename = {}, Utf16View display_filename = {}, GC::Ptr<GC::Cell> host_defined = nullptr, size_t line_number_offset = 0);

    // Fails with one error if the cache turns out not to match the source code.
    static Result<GC::Ref<SourceTextModule>, Vector<ParserError>> parse_from_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache>, NonnullRefPtr<SourceCode const> source_code, Realm&, StringView filename, GC::Ptr<GC::Cell> host_defined = nullptr);

    // A host that generates a bytecode cache for the module marks it as such until it installs the cache, which then
    // replaces the module's bytecode, or gives up.
    void begin_bytecode_cache_generation();
    void finish_bytecode_cache_generation_without_install();
    void install_generated_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache>, NonnullRefPtr<SourceCode const> source_code);
};

}
