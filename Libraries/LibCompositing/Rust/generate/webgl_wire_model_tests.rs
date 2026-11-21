/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tests of the model of the WebGL functions, which run with those of libcompositing_rust only, as every crate
//! whose build script generates from the model includes it.

use crate::webgl_wire_model::*;

fn fields(types_and_names: &[(&str, &str)]) -> Vec<FieldSpec> {
    types_and_names
        .iter()
        .map(|(cpp_type, name)| FieldSpec {
            cpp_type: (*cpp_type).into(),
            name: (*name).into(),
            argument: None,
        })
        .collect()
}

#[test]
fn a_struct_is_laid_out_as_the_c_abi_does() {
    let layout = wire_struct(
        "Commands::Example".into(),
        fields(&[
            ("GLenum", "target"),
            ("GLintptr", "offset"),
            ("GLboolean", "flag"),
            ("WebGLDataSpan", "data"),
        ]),
    )
    .unwrap();
    let offsets: Vec<usize> = layout.fields.iter().map(|field| field.offset).collect();
    assert_eq!(offsets, [0, 8, 16, 20]);
    assert_eq!((layout.size, layout.alignment), (32, 8));
}

#[test]
fn a_struct_without_fields_is_one_byte() {
    let layout = wire_struct("SyncCalls::Finish::Request".into(), Vec::new()).unwrap();
    assert_eq!((layout.size, layout.alignment), (1, 1));
}

#[test]
fn a_field_of_an_unknown_type_is_rejected() {
    let error = wire_struct("Commands::Example".into(), fields(&[("GLdouble", "depth")])).unwrap_err();
    assert!(
        error.contains("Commands::Example.depth has the type GLdouble"),
        "{error}"
    );
}

#[test]
fn names_are_converted_as_the_cpp_generators_convert_them() {
    for (name, snake_case) in [
        ("ActiveTexture", "active_texture"),
        ("TexImage3DRobustANGLE", "tex_image3d_robust_angle"),
        ("GetInteger64vRobustANGLE", "get_integer64v_robust_angle"),
        ("UniformMatrix4fv", "uniform_matrix4fv"),
        ("bufSize", "buf_size"),
        ("srcX0", "src_x0"),
        ("uniformBlockName", "uniform_block_name"),
    ] {
        assert_eq!(title_case_to_snake_case(name), snake_case);
    }
}

#[test]
fn the_structs_of_a_function_follow_from_its_signature() {
    let functions = parse_functions(
        r#"[
            {"name": "glBufferData", "return": "void", "category": "command", "args": [
                {"type": "GLenum", "name": "target"},
                {"type": "GLsizeiptr", "name": "size"},
                {"type": "void const*", "name": "data", "payload": "size", "nullable": true},
                {"type": "GLenum", "name": "usage"}]},
            {"name": "glGetActiveAttrib", "return": "void", "category": "sync", "args": [
                {"type": "GLuint", "name": "program", "object": true},
                {"type": "GLsizei", "name": "bufSize"},
                {"type": "GLsizei*", "name": "length", "out": true},
                {"type": "GLchar*", "name": "name", "out": true, "payload": "bufSize * sizeof(GLchar)"}]},
            {"name": "glCreateShader", "return": "GLuint", "category": "gen", "args": [
                {"type": "GLenum", "name": "type"}]}
        ]"#,
    )
    .unwrap();
    let vocabulary = vocabulary(&functions).unwrap();
    let described = |layout: &WireStruct| -> Vec<String> {
        layout
            .fields
            .iter()
            .map(|field| format!("{} {} @{}", field.spec.cpp_type, field.spec.name, field.offset))
            .collect()
    };
    assert_eq!(vocabulary.commands[0].layout.cpp_name, "Commands::BufferData");
    assert_eq!(
        described(&vocabulary.commands[0].layout),
        [
            "GLenum target @0",
            "GLsizeiptr size @8",
            "bool has_data @16",
            "WebGLDataSpan data @20",
            "GLenum usage @28"
        ]
    );
    assert_eq!(
        described(&vocabulary.commands[1].layout),
        ["WebGLObjectId id @0", "GLenum type @4"]
    );
    assert_eq!(
        vocabulary.sync_calls[0].request.cpp_name,
        "SyncCalls::GetActiveAttrib::Request"
    );
    assert_eq!(
        described(&vocabulary.sync_calls[0].request),
        ["WebGLObjectId program @0", "GLsizei buf_size @4"]
    );
    assert_eq!(
        described(&vocabulary.sync_calls[0].reply),
        ["GLsizei length @0", "WebGLDataSpan name @4"]
    );
}

#[test]
fn what_the_model_does_not_know_is_rejected() {
    let unknown_function_key =
        r#"[{"name": "glFlush", "return": "void", "category": "command", "args": [], "flushes": true}]"#;
    assert!(
        parse_functions(unknown_function_key)
            .unwrap_err()
            .contains("glFlush has the key \"flushes\"")
    );
    let unknown_argument_key = r#"[{"name": "glEnable", "return": "void", "category": "command", "args": [
        {"type": "GLenum", "name": "cap", "validated": true}]}]"#;
    assert!(
        parse_functions(unknown_argument_key)
            .unwrap_err()
            .contains("has the key \"validated\"")
    );
    let unknown_category = r#"[{"name": "glFlush", "return": "void", "category": "deferred", "args": []}]"#;
    assert!(
        parse_functions(unknown_category)
            .unwrap_err()
            .contains("has the category \"deferred\"")
    );
    let unhandled_pointer = r#"[{"name": "glGetPointerv", "return": "void", "category": "command", "args": [
        {"type": "void**", "name": "params"}]}]"#;
    let functions = parse_functions(unhandled_pointer).unwrap();
    assert!(
        vocabulary(&functions)
            .err()
            .unwrap()
            .contains("does not handle the pointer argument params")
    );
}

#[test]
fn what_the_replayer_needs_is_read_from_the_json() {
    let functions = parse_functions(
        r#"[
            {"name": "glDeleteBuffers", "return": "void", "category": "command", "deletes_objects": true, "args": [
                {"type": "GLsizei", "name": "n"},
                {"type": "GLuint const*", "name": "buffers", "object": true, "payload": "n * sizeof(GLuint)"}]},
            {"name": "glBindFramebuffer", "return": "void", "category": "command", "args": [
                {"type": "GLenum", "name": "target"},
                {"type": "GLuint", "name": "framebuffer", "object": true, "zero_means_default": "default_framebuffer"}]},
            {"name": "glClientWaitSync", "return": "GLenum", "category": "sync", "args": [
                {"type": "GLsync", "name": "sync", "object": true},
                {"type": "GLuint64", "name": "timeout", "host_override": "0"}]},
            {"name": "glClearBufferfv", "return": "void", "category": "command", "args": [
                {"type": "GLenum", "name": "buffer"},
                {"type": "GLfloat const*", "name": "value", "payload": "(buffer == GL_COLOR ? 4 : 1) * sizeof(GLfloat)"}]}
        ]"#,
    )
    .unwrap();
    assert!(functions[0].deletes_objects);
    assert!(!functions[1].deletes_objects);
    assert_eq!(
        functions[0].payload_size(&functions[0].arguments[1]).unwrap(),
        [PayloadFactor::Argument(0), PayloadFactor::Constant(4)]
    );
    assert_eq!(
        functions[1].arguments[1].zero_means_default,
        Some(DefaultObject::Framebuffer)
    );
    assert_eq!(functions[2].arguments[1].host_override.as_deref(), Some("0"));
    assert_eq!(
        functions[3].payload_size(&functions[3].arguments[1]).unwrap(),
        [PayloadFactor::ColorBufferComponents(0), PayloadFactor::Constant(4)]
    );
    let unknown_default = r#"[{"name": "glBindTexture", "return": "void", "category": "command", "args": [
        {"type": "GLuint", "name": "texture", "object": true, "zero_means_default": "default_texture"}]}]"#;
    assert!(
        parse_functions(unknown_default)
            .unwrap_err()
            .contains("default object \"default_texture\"")
    );
}
