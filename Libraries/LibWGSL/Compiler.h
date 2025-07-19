/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWGSL/AST.h>
#include <LibWGSL/Export.h>

namespace WGSL {

class WGSL_API Compiler {
public:
    explicit Compiler(StringView source);

    ErrorOr<String> emit_spirv_text();
    ErrorOr<Vector<uint32_t>> emit_spirv_binary(StringView text_assembly);

    struct VertexShader {
        String entry_point;
        String source;
    };
    struct FragmentShader {
        String entry_point;
        String source;
    };
    using Shader = Variant<VertexShader, FragmentShader>;

    ErrorOr<Vector<Shader>> emit_msl();

private:
    ErrorOr<Program> parse();

    StringView m_source;
};

}
