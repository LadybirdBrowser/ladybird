/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Result.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/DecodedBytecodeCache.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/SourceCode.h>

namespace JS {

// 16.1.4 Script Records, https://tc39.es/ecma262/#sec-script-records
class JS_API Script final : public EngineCell {
public:
    static Result<GC::Ref<Script>, Vector<ParserError>> parse(Utf16View source_text, Realm&, StringView filename = {}, Utf16View display_filename = {}, GC::Ptr<GC::Cell> host_defined = nullptr, size_t line_number_offset = 1);

    // Fails with one error if the cache turns out not to match the source code.
    static Result<GC::Ref<Script>, Vector<ParserError>> create_from_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache>, NonnullRefPtr<SourceCode const> source_code, Realm&, StringView filename, GC::Ptr<GC::Cell> host_defined = nullptr);

    GC::Ptr<GC::Cell> host_defined() const;

    // A host that generates a bytecode cache for the script marks it as such until it installs the cache, which then
    // replaces the script's bytecode, or gives up.
    void begin_bytecode_cache_generation();
    void finish_bytecode_cache_generation_without_install();
    void install_generated_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache>, NonnullRefPtr<SourceCode const> source_code);
};

}
