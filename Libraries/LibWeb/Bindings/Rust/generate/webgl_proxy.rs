/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Writes the methods of the WebGL command recorder that follow from the signature of a GL function, which
//! src/web_gl/proxy/generated.rs includes: one per command, per object creation and per synchronous call of
//! GLFunctions.json. A method has the name and the arguments of the GL function, records the struct of the command
//! or makes the round trip of the call, and returns what the function returns.

use std::fmt::Write;

use crate::webgl_wire_model::{
    Argument, Category, FieldSpec, Function, WireStruct, WireType, rust_identifier, title_case_to_snake_case,
    wire_struct,
};

fn parameter_name(argument: &Argument) -> String {
    rust_identifier(&title_case_to_snake_case(&argument.name))
}

fn inline_data_name(argument: &Argument) -> String {
    format!("{}_inline_data", title_case_to_snake_case(&argument.name))
}

fn unsupported(function: &Function, argument: &Argument, what: &str) -> String {
    format!(
        "the recorder has no method for {}, whose argument {} {what}",
        function.name, argument.name
    )
}

fn scalar_parameter_type(function: &Function, argument: &Argument) -> Result<&'static str, String> {
    match WireType::of(&argument.cpp_type) {
        // A GLboolean.
        Some(WireType::U8) => Ok("bool"),
        Some(WireType::DataSpan) | None => Err(unsupported(function, argument, "is not a plain value")),
        Some(wire_type) => Ok(wire_type.rust_type()),
    }
}

fn size_of_gl_type(gl_type: &str) -> Option<usize> {
    Some(match gl_type {
        "GLchar" | "GLboolean" => 1,
        "GLint" | "GLuint" | "GLfloat" | "GLenum" => 4,
        "GLint64" => 8,
        _ => return None,
    })
}

fn payload_size_expression(function: &Function, argument: &Argument) -> Result<String, String> {
    let payload = argument.payload.as_deref().expect("the argument has a payload");
    let mut factors = Vec::new();
    for factor in payload.split(" * ") {
        if factor.bytes().all(|byte| byte.is_ascii_digit()) {
            factors.push(factor.to_string());
        } else if let Some(gl_type) = factor.strip_prefix("sizeof(").and_then(|rest| rest.strip_suffix(')')) {
            let size = size_of_gl_type(gl_type)
                .ok_or_else(|| unsupported(function, argument, &format!("is counted in {gl_type}")))?;
            factors.push(size.to_string());
        } else if factor == "(buffer == GL_COLOR ? 4 : 1)" {
            factors.push("if buffer == GL_COLOR { 4 } else { 1 }".to_string());
        } else {
            let counted_by = function
                .arguments
                .iter()
                .find(|other| other.name == factor && !other.is_out);
            let name = counted_by.map(parameter_name);
            match (name, counted_by.and_then(|other| WireType::of(&other.cpp_type))) {
                (Some(name), Some(WireType::I32)) => factors.push(format!("i64::from({name})")),
                (Some(name), Some(WireType::I64)) => factors.push(name),
                _ => return Err(unsupported(function, argument, &format!("has the size {payload:?}"))),
            }
        }
    }
    Ok(format!("payload_size(&[{}])", factors.join(", ")))
}

#[derive(Default)]
struct Inputs {
    parameters: Vec<String>,
    statements: Vec<String>,
    inline_data: Option<String>,
    /// `None` for a field that has the name of the parameter it is from.
    field_values: Vec<(String, Option<String>)>,
}

impl Inputs {
    fn add_inline_data(&mut self, function: &Function, argument: &Argument, data: String) -> Result<(), String> {
        if self.inline_data.is_some() {
            return Err(unsupported(function, argument, "is a second one that points at data"));
        }
        let name = inline_data_name(argument);
        self.statements.push(format!("let {name} = {data};"));
        self.inline_data = Some(name);
        Ok(())
    }

    fn inline_data(&self) -> &str {
        self.inline_data.as_deref().unwrap_or("InlineData::NONE")
    }

    fn parameter_list(&self) -> String {
        self.parameters
            .iter()
            .map(|parameter| format!(", {parameter}"))
            .collect()
    }
}

fn inputs(function: &Function, struct_path: &str, fields: &[FieldSpec]) -> Result<Inputs, String> {
    let mut inputs = Inputs::default();
    for (index, argument) in function.arguments.iter().enumerate() {
        if argument.is_out {
            continue;
        }
        let name = parameter_name(argument);
        let span = format!("DataSpan::following::<{struct_path}>({})", inline_data_name(argument));
        let fields_of_argument = fields.iter().filter(|field| field.argument == Some(index));
        if argument.is_string {
            inputs.parameters.push(format!("{name}: &[u8]"));
            inputs.add_inline_data(
                function,
                argument,
                format!("InlineData::NulTerminated(until_nul({name}))"),
            )?;
            inputs.field_values.push((name, Some(span)));
        } else if argument.is_offset {
            inputs.parameters.push(format!("{name}: i64"));
            inputs.field_values.push((name, None));
        } else if argument.is_object && !argument.is_pointer() {
            inputs.parameters.push(format!("{name}: u32"));
            inputs.field_values.push((name, None));
        } else if argument.payload.is_some() {
            if !argument.is_const_pointer() {
                return Err(unsupported(function, argument, "points at data that is not const"));
            }
            let size = payload_size_expression(function, argument)?;
            let data = if argument.is_object {
                inputs.parameters.push(format!("{name}: &[u32]"));
                format!("leading_bytes(bytes_of({name}), {size})")
            } else if argument.is_nullable {
                inputs.parameters.push(format!("{name}: Option<&[u8]>"));
                format!("{name}.map(|{name}| leading_bytes({name}, {size})).unwrap_or_default()")
            } else {
                inputs.parameters.push(format!("{name}: &[u8]"));
                format!("leading_bytes({name}, {size})")
            };
            inputs.add_inline_data(function, argument, format!("InlineData::Bytes({data})"))?;
            for field in fields_of_argument {
                if field.cpp_type == "bool" {
                    inputs
                        .field_values
                        .push((rust_identifier(&field.name), Some(format!("{name}.is_some()"))));
                }
            }
            inputs.field_values.push((name, Some(span)));
        } else if argument.is_pointer() {
            return Err(unsupported(function, argument, "is a pointer without a size"));
        } else {
            let parameter_type = scalar_parameter_type(function, argument)?;
            inputs.parameters.push(format!("{name}: {parameter_type}"));
            let value = (argument.cpp_type == "GLboolean").then(|| format!("u8::from({name})"));
            inputs.field_values.push((name, value));
        }
    }
    Ok(inputs)
}

fn struct_expression(struct_path: &str, layout: &WireStruct, field_values: &[(String, Option<String>)]) -> String {
    if layout.fields.is_empty() {
        return format!("{struct_path}::ZEROED");
    }
    let mut expression = format!("{struct_path} {{ ");
    for (name, value) in field_values {
        match value {
            Some(value) => write!(expression, "{name}: {value}, ").unwrap(),
            None => write!(expression, "{name}, ").unwrap(),
        }
    }
    if layout.has_unused_bytes() {
        write!(expression, "..{struct_path}::ZEROED ").unwrap();
    }
    expression.push('}');
    expression
}

fn check_fields_have_values(
    function: &Function,
    layout: &WireStruct,
    field_values: &[(String, Option<String>)],
) -> Result<(), String> {
    let mut field_names: Vec<String> = layout
        .fields
        .iter()
        .map(|field| rust_identifier(&field.spec.name))
        .collect();
    let mut value_names: Vec<String> = field_values.iter().map(|(name, _)| name.clone()).collect();
    field_names.sort();
    value_names.sort();
    if field_names == value_names {
        Ok(())
    } else {
        Err(format!(
            "the recorder would fill the fields {value_names:?} of {}, which has the fields {field_names:?}",
            function.name
        ))
    }
}

fn write_statements(out: &mut String, statements: &[String]) {
    for statement in statements {
        writeln!(out, "        {statement}").unwrap();
    }
}

fn write_command_method(out: &mut String, function: &Function) -> Result<(), String> {
    let name = function.command_name();
    let struct_path = format!("commands::{name}");
    let fields = function.command_fields()?;
    let layout = wire_struct(format!("Commands::{name}"), fields.clone())?;
    let inputs = inputs(function, &struct_path, &fields)?;
    check_fields_have_values(function, &layout, &inputs.field_values)?;

    writeln!(
        out,
        "    pub(crate) fn {}(&self{}) {{",
        function.method_name(),
        inputs.parameter_list()
    )
    .unwrap();
    write_statements(out, &inputs.statements);
    let command = struct_expression(&struct_path, &layout, &inputs.field_values);
    match &inputs.inline_data {
        Some(inline_data) => writeln!(out, "        self.record_with_data(&{command}, {inline_data});").unwrap(),
        None => writeln!(out, "        self.record(&{command});").unwrap(),
    }
    out.push_str("    }\n\n");
    Ok(())
}

/// The recorder allocates the ids of the objects, so that it does not have to wait for the replayer to create them.
fn write_gen_method(out: &mut String, function: &Function) -> Result<(), String> {
    let name = function.command_name();
    let struct_path = format!("commands::{name}");
    let fields = function.command_fields()?;
    let layout = wire_struct(format!("Commands::{name}"), fields.clone())?;
    let method_name = function.method_name();

    if !function.returns_void() {
        let mut inputs = inputs(function, &struct_path, &fields)?;
        if inputs.inline_data.is_some() {
            return Err(format!(
                "the recorder has no method for {}, which creates an object from data",
                function.name
            ));
        }
        inputs.field_values.insert(0, ("id".to_string(), None));
        check_fields_have_values(function, &layout, &inputs.field_values)?;
        writeln!(
            out,
            "    pub(crate) fn {method_name}(&self{}) -> u32 {{",
            inputs.parameter_list()
        )
        .unwrap();
        out.push_str("        let id = self.allocate_object_id();\n");
        let command = struct_expression(&struct_path, &layout, &inputs.field_values);
        writeln!(out, "        self.record(&{command});").unwrap();
        out.push_str("        id\n    }\n\n");
        return Ok(());
    }

    let [count, ids] = function.arguments.as_slice() else {
        return Err(format!(
            "the recorder has no method for {}, which is not a count and ids",
            function.name
        ));
    };
    if WireType::of(&count.cpp_type) != Some(WireType::I32) || ids.cpp_type != "GLuint*" {
        return Err(format!(
            "the recorder has no method for {}, which is not a count and ids",
            function.name
        ));
    }
    let count_name = parameter_name(count);
    let ids_name = parameter_name(ids);
    let inline_data = inline_data_name(ids);
    let field_values = [
        (count_name, Some(format!("gen_count({ids_name})"))),
        (
            ids_name.clone(),
            Some(format!("DataSpan::following::<{struct_path}>({inline_data})")),
        ),
    ];
    check_fields_have_values(function, &layout, &field_values)?;
    writeln!(out, "    pub(crate) fn {method_name}(&self, {ids_name}: &mut [u32]) {{").unwrap();
    writeln!(out, "        if {ids_name}.is_empty() {{").unwrap();
    out.push_str("            return;\n        }\n");
    writeln!(out, "        for id in &mut *{ids_name} {{").unwrap();
    out.push_str("            *id = self.allocate_object_id();\n        }\n");
    writeln!(
        out,
        "        let {inline_data} = InlineData::Bytes(bytes_of({ids_name}));"
    )
    .unwrap();
    let command = struct_expression(&struct_path, &layout, &field_values);
    writeln!(out, "        self.record_with_data(&{command}, {inline_data});").unwrap();
    out.push_str("    }\n\n");
    Ok(())
}

fn output_element_type(function: &Function, argument: &Argument) -> Result<&'static str, String> {
    match argument.pointee_cpp_type().as_str() {
        "GLchar" | "GLboolean" => Ok("u8"),
        "GLint" | "GLsizei" => Ok("i32"),
        "GLuint" | "GLenum" => Ok("u32"),
        "GLfloat" => Ok("f32"),
        "GLint64" => Ok("i64"),
        other => Err(unsupported(function, argument, &format!("is a buffer of {other}"))),
    }
}

/// A value that a synchronous call returns: the return value of the GL function, or what it stores through a
/// pointer to one value.
struct ScalarOutput {
    name: String,
    rust_type: &'static str,
    value: String,
}

fn scalar_output(function: &Function, field: &FieldSpec) -> Result<ScalarOutput, String> {
    let name = rust_identifier(&field.name);
    let no_rust_type = || {
        format!(
            "the recorder has no method for {}, which returns a {}",
            function.name, field.cpp_type
        )
    };
    let (rust_type, value) = match WireType::of(&field.cpp_type).ok_or_else(no_rust_type)? {
        WireType::DataSpan | WireType::Bool => return Err(no_rust_type()),
        // A GLboolean.
        WireType::U8 => ("bool", format!("reply.{name} != 0")),
        wire_type => (wire_type.rust_type(), format!("reply.{name}")),
    };
    Ok(ScalarOutput { name, rust_type, value })
}

fn write_sync_method(out: &mut String, output_structs: &mut String, function: &Function) -> Result<(), String> {
    let name = function.command_name();
    let struct_path = format!("sync_calls::{name}Request");
    let request_fields = function.sync_request_fields()?;
    let request_layout = wire_struct(format!("SyncCalls::{name}::Request"), request_fields.clone())?;
    let mut inputs = inputs(function, &struct_path, &request_fields)?;
    check_fields_have_values(function, &request_layout, &inputs.field_values)?;

    let mut scalar_outputs = Vec::new();
    let mut copies = Vec::new();
    for field in function.sync_reply_fields() {
        let argument = field.argument.map(|index| &function.arguments[index]);
        match argument {
            Some(argument) if argument.payload.is_some() => {
                let parameter = parameter_name(argument);
                let element_type = output_element_type(function, argument)?;
                inputs.parameters.push(format!("{parameter}: &mut [{element_type}]"));
                let size = payload_size_expression(function, argument)?;
                copies.push(format!(
                    "copy_reply_data(reply_bytes, reply.{}, leading_bytes_mut(bytes_of_mut({parameter}), {size}));",
                    rust_identifier(&field.name)
                ));
            }
            _ => scalar_outputs.push(scalar_output(function, &field)?),
        }
    }

    let (return_type, output) = match scalar_outputs.as_slice() {
        [] => (String::new(), None),
        [output] => (format!(" -> {}", output.rust_type), Some(output.value.clone())),
        outputs => {
            let struct_name = format!("{name}Outputs");
            writeln!(output_structs, "/// What `{}` returns.", function.name).unwrap();
            output_structs.push_str("#[derive(Clone, Copy, Debug, Default, PartialEq)]\n");
            writeln!(output_structs, "pub(crate) struct {struct_name} {{").unwrap();
            let mut values = String::new();
            for output in outputs {
                writeln!(output_structs, "    pub(crate) {}: {},", output.name, output.rust_type).unwrap();
                write!(values, "{}: {}, ", output.name, output.value).unwrap();
            }
            output_structs.push_str("}\n\n");
            (
                format!(" -> {struct_name}"),
                Some(format!("{struct_name} {{ {values}}}")),
            )
        }
    };

    writeln!(
        out,
        "    pub(crate) fn {}(&self{}){return_type} {{",
        function.method_name(),
        inputs.parameter_list()
    )
    .unwrap();
    write_statements(out, &inputs.statements);
    let request = struct_expression(&struct_path, &request_layout, &inputs.field_values);
    writeln!(out, "        let request = {request};").unwrap();
    let reply = if output.is_some() || !copies.is_empty() {
        "reply"
    } else {
        "_reply"
    };
    let reply_bytes = if copies.is_empty() {
        "_reply_bytes"
    } else {
        "reply_bytes"
    };
    writeln!(
        out,
        "        self.sync_call(&request, {}, |{reply}, {reply_bytes}| {{",
        inputs.inline_data()
    )
    .unwrap();
    for copy in &copies {
        writeln!(out, "            {copy}").unwrap();
    }
    if let Some(output) = &output {
        writeln!(out, "            {output}").unwrap();
    }
    // NB: Without a reply, which is when the context is lost, a call returns zeros and leaves the buffers alone.
    out.push_str(if output.is_some() {
        "        })\n        .unwrap_or_default()\n"
    } else {
        "        });\n"
    });
    out.push_str("    }\n\n");
    Ok(())
}

pub fn write_proxy(functions: &[Function]) -> Result<String, String> {
    let mut methods = String::new();
    let mut output_structs = String::new();
    for function in functions {
        match function.category {
            Category::Command => write_command_method(&mut methods, function)?,
            Category::Gen => write_gen_method(&mut methods, function)?,
            Category::Sync => write_sync_method(&mut methods, &mut output_structs, function)?,
            // Written by hand in src/web_gl/proxy/custom.rs.
            Category::Custom | Category::BuiltinCommand => {}
        }
    }

    let mut out = String::from("// Generated by build.rs from Libraries/LibCompositing/WebGL/GLFunctions.json.\n\n");
    out.push_str(&output_structs);
    out.push_str(
        "#[allow(clippy::too_many_arguments, reason = \"the methods have the arguments of the GL functions\")]\n",
    );
    out.push_str("impl<T: Transport> Proxy<T> {\n");
    out.push_str(methods.trim_end());
    out.push_str("\n}\n");
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::webgl_wire_model::parse_functions;

    fn proxy_for(function_json: &str) -> Result<String, String> {
        write_proxy(&parse_functions(&format!("[{function_json}]")).unwrap())
    }

    #[test]
    fn the_size_of_a_payload_is_the_product_that_the_json_gives() {
        let proxy = proxy_for(
            r#"{"name": "glUniformMatrix4fv", "return": "void", "category": "command", "args": [
                {"type": "GLint", "name": "location"},
                {"type": "GLsizei", "name": "count"},
                {"type": "GLboolean", "name": "transpose"},
                {"type": "GLfloat const*", "name": "value", "payload": "count * 16 * sizeof(GLfloat)"}]}"#,
        )
        .unwrap();
        assert!(proxy.contains(
            "pub(crate) fn uniform_matrix4fv(&self, location: i32, count: i32, transpose: bool, value: &[u8]) {"
        ));
        assert!(proxy.contains("leading_bytes(value, payload_size(&[i64::from(count), 16, 4]))"));
        assert!(proxy.contains("transpose: u8::from(transpose), value: DataSpan::following::<"));
    }

    #[test]
    fn a_synchronous_call_returns_its_outputs_and_fills_its_buffers() {
        let proxy = proxy_for(
            r#"{"name": "glGetActiveAttrib", "return": "void", "category": "sync", "args": [
                {"type": "GLuint", "name": "program", "object": true},
                {"type": "GLsizei", "name": "bufSize"},
                {"type": "GLsizei*", "name": "length", "out": true},
                {"type": "GLenum*", "name": "type", "out": true},
                {"type": "GLchar*", "name": "name", "out": true, "payload": "bufSize * sizeof(GLchar)"}]}"#,
        )
        .unwrap();
        assert!(proxy.contains(
            "pub(crate) struct GetActiveAttribOutputs {\n    pub(crate) length: i32,\n    pub(crate) r#type: u32,\n}"
        ));
        assert!(proxy.contains(
            "pub(crate) fn get_active_attrib(&self, program: u32, buf_size: i32, name: &mut [u8]) -> GetActiveAttribOutputs {"
        ));
        assert!(proxy.contains("copy_reply_data(reply_bytes, reply.name, leading_bytes_mut(bytes_of_mut(name), payload_size(&[i64::from(buf_size), 1])));"));
        assert!(proxy.contains("GetActiveAttribOutputs { length: reply.length, r#type: reply.r#type, }"));
    }

    #[test]
    fn what_the_recorder_cannot_serialize_is_rejected() {
        let two_payloads = proxy_for(
            r#"{"name": "glTwo", "return": "void", "category": "command", "args": [
                {"type": "GLsizei", "name": "n"},
                {"type": "GLint const*", "name": "a", "payload": "n * sizeof(GLint)"},
                {"type": "GLint const*", "name": "b", "payload": "n * sizeof(GLint)"}]}"#,
        );
        assert!(
            two_payloads
                .unwrap_err()
                .contains("is a second one that points at data")
        );
        let unknown_size = proxy_for(
            r#"{"name": "glSized", "return": "void", "category": "command", "args": [
                {"type": "GLint const*", "name": "a", "payload": "strlen(a)"}]}"#,
        );
        assert!(unknown_size.unwrap_err().contains("has the size \"strlen(a)\""));
        let unknown_element = proxy_for(
            r#"{"name": "glGetDoubles", "return": "void", "category": "sync", "args": [
                {"type": "GLsizei", "name": "n"},
                {"type": "GLdouble*", "name": "values", "out": true, "payload": "n * sizeof(GLint64)"}]}"#,
        );
        assert!(unknown_element.unwrap_err().contains("is a buffer of GLdouble"));
    }
}
