#!/usr/bin/env python3

# Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent))

from libweb_webgl import method_signature
from libweb_webgl import run_generator

# Generates Web::WebGL::GLFunctions from GLFunctions.json: one member function per GL
# entry point used by the host WebGL implementation. This is the only place that is
# allowed to call GL entry points directly; everything above it goes through the methods
# so the GL boundary stays in one generated, mechanically-verifiable layer.


def write_header_file(out: TextIO, functions: list) -> None:
    out.write("""#pragma once

// GL type and enum definitions only -- GL_GLEXT_PROTOTYPES stays undefined here so that
// nothing outside the generated GLFunctions implementation can call GL directly.
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}
#include <GLES3/gl3.h>

#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class GLFunctions {
public:
""")

    for function in functions:
        if function["category"].startswith("builtin"):
            continue
        out.write(f"    {method_signature(function)};\n")

    out.write("""};

}
""")


def write_implementation_file(out: TextIO, functions: list) -> None:
    out.write("""#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}
#include <GLES3/gl3.h>

#include <LibWeb/WebGL/GLFunctions.h>

namespace Web::WebGL {
""")

    for function in functions:
        if function["category"].startswith("builtin"):
            continue
        forwarded_args = ", ".join(arg["name"] for arg in function["args"])
        call = f"::{function['name']}({forwarded_args})"
        if function["return"] != "void":
            call = "return " + call
        out.write(f"""
{method_signature(function, "GLFunctions::")}
{{
    {call};
}}
""")

    out.write("""
}
""")


if __name__ == "__main__":
    run_generator("Generate WebGL GL function wrappers", write_header_file, write_implementation_file)
