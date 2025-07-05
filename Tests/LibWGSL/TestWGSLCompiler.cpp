/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWGSL/Compiler.h>

constexpr auto triangle_shader = R"(
struct VertexIn {
  @location(0) position: vec4f,
  @location(1) color: vec4f,
};

struct VertexOut {
  @builtin(position) position : vec4f,
  @location(0) color : vec4f
}

@vertex
fn vertex_main(input: VertexIn) -> VertexOut {
  var output : VertexOut;
  output.position = input.position;
  output.color = input.color;
  return output;
}

@fragment
fn fragment_main(fragData: VertexOut) -> @location(0) vec4f {
  return fragData.color;
}
)"sv;

void test_shader_lines_equal(Vector<String> const& actual_lines, Vector<StringView> const& expected_lines);
void test_shader_lines_equal(Vector<String> const& actual_lines, Vector<StringView> const& expected_lines)
{
    size_t const num_actual_lines = actual_lines.size();
    if (size_t const num_expected_lines = expected_lines.size(); num_actual_lines != num_expected_lines) {
        FAIL(String::formatted("actual line count: {}, expected line count: {}", num_actual_lines, num_expected_lines));
        return;
    }

    for (size_t i = 0; i < num_actual_lines; ++i) {
        auto const& actual_line = actual_lines[i];
        if (auto const& expected_line = expected_lines[i]; actual_line != expected_line) {
            FAIL(String::formatted("index[{}]: actual line: {}, expected line: {}", i, actual_line, expected_line));
        }
    }
}

TEST_CASE(compile_simple_triangle_shader_spirv)
{
    constexpr auto expected_triangle_shader_spirv_text = R"(; Magic:     0x07230203 (SPIR-V)
; Version:   0x00010600 (Version: 1.6.0)
; Generator: 0xFFFF0001 (Ladybird LibWGSL; 1)
; Bound:     100
; Schema:    0
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %14 "vertex_main" %7 %9 %11 %13
OpEntryPoint Fragment %30 "fragment_main" %25 %27 %29
OpExecutionMode %30 OriginLowerLeft
OpSource WGSL 100
OpDecorate %11 Location 0
OpDecorate %13 BuiltIn Position
OpDecorate %29 Location 0
OpDecorate %7 Location 1
OpDecorate %9 Location 0
OpDecorate %25 Location 0
OpDecorate %27 BuiltIn Position
%1 = OpTypeVoid
%2 = OpTypeFloat 32
%3 = OpTypeInt 32 1
%4 = OpTypeVector %2 4
%5 = OpTypeFunction %1
%6 = OpTypePointer Input %4
%7 = OpVariable %6 Input
%8 = OpTypePointer Input %4
%9 = OpVariable %8 Input
%10 = OpTypePointer Output %4
%11 = OpVariable %10 Output
%12 = OpTypePointer Output %4
%13 = OpVariable %12 Output
%16 = OpTypePointer Function %4
%18 = OpTypePointer Function %4
%24 = OpTypePointer Input %4
%25 = OpVariable %24 Input
%26 = OpTypePointer Input %4
%27 = OpVariable %26 Input
%28 = OpTypePointer Output %4
%29 = OpVariable %28 Output
%14 = OpFunction %1 None %5
%15 = OpLabel
%17 = OpVariable %16 Function
%19 = OpVariable %18 Function
%20 = OpLoad %4 %7
%21 = OpLoad %4 %9
OpStore %19 %21
OpStore %17 %20
%22 = OpLoad %4 %17
%23 = OpLoad %4 %19
OpStore %11 %22
OpStore %13 %23
OpReturn
OpFunctionEnd
%30 = OpFunction %1 None %5
%31 = OpLabel
%32 = OpLoad %4 %25
%33 = OpLoad %4 %27
OpStore %29 %32
OpReturn
OpFunctionEnd)"sv;

    Vector const expected_triangle_shader_spirv_lines = expected_triangle_shader_spirv_text.split_view('\n');
    constexpr size_t expected_optimized_spriv_binary_size = 158;

    WGSL::Compiler compiler(triangle_shader);
    auto spirv_text_result = compiler.emit_spirv_text();
    if (spirv_text_result.is_error()) {
        FAIL("Unable to compile WGSL triangle shader source code to SPIRV text");
        return;
    }
    String const& spirv_text = spirv_text_result.value();
    Vector const actual_spirv_text_lines = MUST(spirv_text.split('\n'));
    test_shader_lines_equal(actual_spirv_text_lines, expected_triangle_shader_spirv_lines);

    auto spirv_binary_result = compiler.emit_spirv_binary(spirv_text);
    if (spirv_binary_result.is_error()) {
        FAIL("Unable to validate and optimize triangle shader SPIRV text to SPIRV binary");
        return;
    }
    Vector<uint32_t> const& spirv_binary = spirv_binary_result.value();
    EXPECT(spirv_binary.size() == expected_optimized_spriv_binary_size);
}

TEST_CASE(compile_simple_triangle_shader_msl)
{
    constexpr auto expected_triangle_shader_msl_vertex_source = R"(#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct fragment_main_out
{
    float4 m_62 [[user(locn0)]];
};

struct fragment_main_in
{
    float4 m_58 [[attribute(0)]];
};

struct vertex_main_out
{
    float4 m_44 [[user(locn0)]];
    float4 gl_Position [[position]];
};

struct vertex_main_in
{
    float4 m_42 [[attribute(0)]];
    float4 m_40 [[attribute(1)]];
};

vertex vertex_main_out vertex_main(vertex_main_in in [[stage_in]])
{
    vertex_main_out out = {};
    out.m_44 = in.m_40;
    out.gl_Position = in.m_42;
    return out;
}
)"sv;
    Vector const expected_triangle_shader_msl_vertex_lines = expected_triangle_shader_msl_vertex_source.split_view('\n');

    constexpr auto expected_triangle_shader_msl_fragment_source = R"(#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct fragment_main_out
{
    float4 m_62 [[color(0)]];
};

struct fragment_main_in
{
    float4 m_58 [[user(locn0)]];
};

fragment fragment_main_out fragment_main(fragment_main_in in [[stage_in]])
{
    fragment_main_out out = {};
    out.m_62 = in.m_58;
    return out;
}
)"sv;
    Vector const expected_triangle_shader_msl_fragment_lines = expected_triangle_shader_msl_fragment_source.split_view('\n');

    WGSL::Compiler compiler(triangle_shader);
    auto const msl_shader_sources_result = compiler.emit_msl();
    if (msl_shader_sources_result.is_error()) {
        FAIL("Unable to compile WGSL triangle shader source code to MSL source");
        return;
    }
    auto const& msl_shader_sources = msl_shader_sources_result.value();
    for (auto const& msl_shader_source : msl_shader_sources) {
        msl_shader_source.visit(
            [&expected_triangle_shader_msl_vertex_lines](WGSL::Compiler::VertexShader const& vertex_shader) {
                Vector const actual_msl_vertex_lines = MUST(vertex_shader.source.split('\n'));
                test_shader_lines_equal(actual_msl_vertex_lines, expected_triangle_shader_msl_vertex_lines);
            },
            [&expected_triangle_shader_msl_fragment_lines](WGSL::Compiler::FragmentShader const& fragment_shader) {
                Vector const actual_msl_fragment_lines = MUST(fragment_shader.source.split('\n'));
                test_shader_lines_equal(actual_msl_fragment_lines, expected_triangle_shader_msl_fragment_lines);
            });
    }
}
