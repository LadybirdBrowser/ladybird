/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The model of Libraries/LibCompositing/WebGL/GLFunctions.json: which entries the command stream carries and which
//! structs they serialize to. It mirrors Meta/Generators/libweb_webgl.py, which the C++ side of the stream is
//! generated from, so the two have to change together. What keeps them from drifting apart silently: the model
//! rejects everything in the JSON that it does not know, and the recorder compares the layout of its structs with
//! the one that the C++ side reports before it records anything.

use serde_json::{Map, Value};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Category {
    Command,
    Gen,
    Sync,
    Custom,
    BuiltinCommand,
}

#[derive(Debug)]
pub struct Argument {
    pub cpp_type: String,
    pub name: String,
    /// The C++ expression for the number of bytes that a pointer argument points at.
    pub payload: Option<String>,
    pub is_object: bool,
    pub is_out: bool,
    pub is_offset: bool,
    pub is_string: bool,
    pub is_nullable: bool,
}

impl Argument {
    pub fn is_pointer(&self) -> bool {
        self.cpp_type.ends_with('*')
    }

    pub fn is_const_pointer(&self) -> bool {
        self.is_pointer() && self.cpp_type.contains("const")
    }

    pub fn pointee_cpp_type(&self) -> String {
        self.cpp_type.replace('*', "").trim().to_string()
    }
}

/// A field of a struct of the command stream, before it has a place in the struct.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FieldSpec {
    pub cpp_type: String,
    pub name: String,
    /// The index of the argument that the field carries, for a field that follows from the signature of the function.
    pub argument: Option<usize>,
}

#[derive(Debug)]
pub struct Function {
    pub name: String,
    pub category: Category,
    /// The built-in commands, which are no GL functions, have none.
    pub return_type: Option<String>,
    pub arguments: Vec<Argument>,
    wire_command: Option<Vec<FieldSpec>>,
    wire_request: Option<Vec<FieldSpec>>,
    wire_reply: Option<Vec<FieldSpec>>,
}

const FUNCTION_KEYS: [&str; 8] = [
    "name",
    "category",
    "return",
    "args",
    // What the replayer does with the objects that a function deletes, which the recorder has no part in.
    "deletes_objects",
    "wire_command",
    "wire_request",
    "wire_reply",
];

const ARGUMENT_KEYS: [&str; 10] = [
    "type",
    "name",
    "payload",
    "object",
    "out",
    "offset",
    "string",
    "nullable",
    // How the replayer resolves an argument, which the recorder has no part in.
    "zero_means_default",
    "host_override",
];

fn reject_unknown_keys(object: &Map<String, Value>, known_keys: &[&str], what: &str) -> Result<(), String> {
    match object.keys().find(|key| !known_keys.contains(&key.as_str())) {
        Some(key) => Err(format!(
            "{what} has the key {key:?}, which the model of the WebGL functions does not know"
        )),
        None => Ok(()),
    }
}

fn string_member(object: &Map<String, Value>, key: &str, what: &str) -> Result<Option<String>, String> {
    match object.get(key) {
        None => Ok(None),
        Some(Value::String(string)) => Ok(Some(string.clone())),
        Some(other) => Err(format!("{what} has {other} for {key:?}, which is not a string")),
    }
}

fn required_string_member(object: &Map<String, Value>, key: &str, what: &str) -> Result<String, String> {
    string_member(object, key, what)?.ok_or_else(|| format!("{what} has no {key:?}"))
}

fn flag(object: &Map<String, Value>, key: &str, what: &str) -> Result<bool, String> {
    match object.get(key) {
        None => Ok(false),
        Some(Value::Bool(true)) => Ok(true),
        Some(other) => Err(format!("{what} has {other} for {key:?}, which can only be true")),
    }
}

fn parse_argument(value: &Value, function_name: &str) -> Result<Argument, String> {
    let what = format!("an argument of {function_name}");
    let object = value.as_object().ok_or_else(|| format!("{what} is not an object"))?;
    reject_unknown_keys(object, &ARGUMENT_KEYS, &what)?;
    Ok(Argument {
        cpp_type: required_string_member(object, "type", &what)?,
        name: required_string_member(object, "name", &what)?,
        payload: string_member(object, "payload", &what)?,
        is_object: flag(object, "object", &what)?,
        is_out: flag(object, "out", &what)?,
        is_offset: flag(object, "offset", &what)?,
        is_string: flag(object, "string", &what)?,
        is_nullable: flag(object, "nullable", &what)?,
    })
}

fn parse_wire_fields(
    object: &Map<String, Value>,
    key: &str,
    function_name: &str,
) -> Result<Option<Vec<FieldSpec>>, String> {
    let Some(value) = object.get(key) else {
        return Ok(None);
    };
    let what = format!("{key:?} of {function_name}");
    let fields = value.as_array().ok_or_else(|| format!("{what} is not an array"))?;
    let mut field_specs = Vec::new();
    for field in fields {
        let field = field
            .as_object()
            .ok_or_else(|| format!("a field of {what} is not an object"))?;
        reject_unknown_keys(field, &["type", "name"], &format!("a field of {what}"))?;
        field_specs.push(FieldSpec {
            cpp_type: required_string_member(field, "type", &what)?,
            name: required_string_member(field, "name", &what)?,
            argument: None,
        });
    }
    Ok(Some(field_specs))
}

fn parse_function(value: &Value) -> Result<Function, String> {
    let object = value
        .as_object()
        .ok_or("an entry of the WebGL functions is not an object")?;
    let name = required_string_member(object, "name", "an entry of the WebGL functions")?;
    reject_unknown_keys(object, &FUNCTION_KEYS, &name)?;
    let category = match required_string_member(object, "category", &name)?.as_str() {
        "command" => Category::Command,
        "gen" => Category::Gen,
        "sync" => Category::Sync,
        "custom" => Category::Custom,
        "builtin-command" => Category::BuiltinCommand,
        other => {
            return Err(format!(
                "{name} has the category {other:?}, which the model of the WebGL functions does not know"
            ));
        }
    };
    let mut arguments = Vec::new();
    if let Some(values) = object.get("args") {
        let values = values
            .as_array()
            .ok_or_else(|| format!("\"args\" of {name} is not an array"))?;
        for value in values {
            arguments.push(parse_argument(value, &name)?);
        }
    }
    let function = Function {
        return_type: string_member(object, "return", &name)?,
        arguments,
        wire_command: parse_wire_fields(object, "wire_command", &name)?,
        wire_request: parse_wire_fields(object, "wire_request", &name)?,
        wire_reply: parse_wire_fields(object, "wire_reply", &name)?,
        category,
        name,
    };
    function.check_shape()?;
    Ok(function)
}

/// In the order of the JSON, which is the order of the command types.
pub fn parse_functions(json: &str) -> Result<Vec<Function>, String> {
    let value: Value =
        serde_json::from_str(json).map_err(|error| format!("the WebGL functions are not JSON: {error}"))?;
    let entries = value.as_array().ok_or("the WebGL functions are not an array")?;
    entries.iter().map(parse_function).collect()
}

impl Function {
    fn check_shape(&self) -> Result<(), String> {
        let name = &self.name;
        let has_signature = self.return_type.is_some();
        let has_wire_structs = self.wire_command.is_some() || self.wire_request.is_some() || self.wire_reply.is_some();
        match self.category {
            Category::BuiltinCommand => {
                if has_signature || !self.arguments.is_empty() || self.wire_command.is_none() {
                    return Err(format!(
                        "{name} is a built-in command, which is its \"wire_command\" and nothing else"
                    ));
                }
            }
            Category::Command | Category::Gen | Category::Sync => {
                if !has_signature || has_wire_structs {
                    return Err(format!(
                        "the structs of {name} follow from its signature, which is all it can have"
                    ));
                }
            }
            Category::Custom => {
                if !has_signature {
                    return Err(format!("{name} has no \"return\""));
                }
            }
        }
        if self.wire_request.is_some() != self.wire_reply.is_some() {
            return Err(format!("{name} has only one of \"wire_request\" and \"wire_reply\""));
        }
        if self.wire_command.is_some() && self.wire_request.is_some() {
            return Err(format!("{name} is both a command and a synchronous call"));
        }
        if !self.is_builtin() && !self.name.starts_with("gl") {
            return Err(format!("the name of {name} does not start with \"gl\""));
        }
        Ok(())
    }

    fn is_builtin(&self) -> bool {
        self.category == Category::BuiltinCommand
    }

    pub fn returns_void(&self) -> bool {
        self.return_type.as_deref() == Some("void")
    }

    pub fn command_name(&self) -> &str {
        if self.is_builtin() { &self.name } else { &self.name[2..] }
    }

    pub fn method_name(&self) -> String {
        title_case_to_snake_case(self.command_name())
    }

    pub fn is_command_stream_entry(&self) -> bool {
        matches!(self.category, Category::Command | Category::Gen) || self.wire_command.is_some()
    }

    pub fn is_sync_call(&self) -> bool {
        self.category == Category::Sync || self.wire_request.is_some()
    }

    fn unhandled_pointer(&self, argument: &Argument) -> String {
        format!(
            "the model of the WebGL functions does not handle the pointer argument {} of {}",
            argument.name, self.name
        )
    }

    pub fn command_fields(&self) -> Result<Vec<FieldSpec>, String> {
        if let Some(fields) = &self.wire_command {
            return Ok(fields.clone());
        }
        let is_gen = self.category == Category::Gen;
        let mut fields = Vec::new();
        if is_gen && !self.returns_void() {
            fields.push(FieldSpec {
                cpp_type: "WebGLObjectId".into(),
                name: "id".into(),
                argument: None,
            });
        }
        for (index, argument) in self.arguments.iter().enumerate() {
            let name = title_case_to_snake_case(&argument.name);
            let cpp_type = if is_gen && argument.is_pointer() && !argument.is_const_pointer() {
                // The ids that the recorder allocated.
                "WebGLDataSpan"
            } else if argument.is_object && !argument.is_pointer() {
                "WebGLObjectId"
            } else if (argument.is_object && argument.is_const_pointer()) || argument.is_string {
                "WebGLDataSpan"
            } else if argument.is_offset {
                "GLintptr"
            } else if argument.payload.is_some() {
                if argument.is_nullable {
                    fields.push(FieldSpec {
                        cpp_type: "bool".into(),
                        name: format!("has_{name}"),
                        argument: Some(index),
                    });
                }
                "WebGLDataSpan"
            } else if argument.is_pointer() {
                return Err(self.unhandled_pointer(argument));
            } else {
                &argument.cpp_type
            };
            fields.push(FieldSpec {
                cpp_type: cpp_type.to_string(),
                name,
                argument: Some(index),
            });
        }
        Ok(fields)
    }

    pub fn sync_request_fields(&self) -> Result<Vec<FieldSpec>, String> {
        if let Some(fields) = &self.wire_request {
            return Ok(fields.clone());
        }
        let mut fields = Vec::new();
        for (index, argument) in self.arguments.iter().enumerate() {
            if argument.is_out {
                continue;
            }
            let cpp_type = if argument.is_object && !argument.is_pointer() {
                "WebGLObjectId"
            } else if argument.is_string || argument.payload.is_some() {
                "WebGLDataSpan"
            } else if argument.is_pointer() {
                return Err(self.unhandled_pointer(argument));
            } else {
                &argument.cpp_type
            };
            fields.push(FieldSpec {
                cpp_type: cpp_type.to_string(),
                name: title_case_to_snake_case(&argument.name),
                argument: Some(index),
            });
        }
        Ok(fields)
    }

    /// The return value, then every output, a buffer as a span into the data that follows the struct.
    pub fn sync_reply_fields(&self) -> Vec<FieldSpec> {
        if let Some(fields) = &self.wire_reply {
            return fields.clone();
        }
        let mut fields = Vec::new();
        if let Some(return_type) = &self.return_type
            && return_type != "void"
        {
            fields.push(FieldSpec {
                cpp_type: return_type.clone(),
                name: "return_value".into(),
                argument: None,
            });
        }
        for (index, argument) in self.arguments.iter().enumerate() {
            if !argument.is_out {
                continue;
            }
            let cpp_type = if argument.payload.is_some() {
                "WebGLDataSpan".to_string()
            } else {
                argument.pointee_cpp_type()
            };
            fields.push(FieldSpec {
                cpp_type,
                name: title_case_to_snake_case(&argument.name),
                argument: Some(index),
            });
        }
        fields
    }
}

/// Mirrors title_case_to_snake_case() in Meta/Utils/utils.py.
pub fn title_case_to_snake_case(value: &str) -> String {
    let characters: Vec<char> = value.chars().collect();
    let mut snake_case = String::new();
    for (index, &character) in characters.iter().enumerate() {
        if character.is_uppercase() && index > 0 {
            let previous_is_lowercase = characters[index - 1].is_lowercase();
            let next_is_lowercase = characters.get(index + 1).is_some_and(|next| next.is_lowercase());
            if previous_is_lowercase || next_is_lowercase {
                snake_case.push('_');
            }
        }
        snake_case.extend(character.to_lowercase());
    }
    snake_case
}

/// The Rust identifier for a name of the JSON, which can be a keyword.
pub fn rust_identifier(name: &str) -> String {
    const KEYWORDS: [&str; 6] = ["type", "ref", "match", "move", "loop", "in"];
    if KEYWORDS.contains(&name) {
        format!("r#{name}")
    } else {
        name.to_string()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WireType {
    U8,
    Bool,
    U32,
    I32,
    F32,
    I64,
    U64,
    DataSpan,
}

impl WireType {
    pub fn of(cpp_type: &str) -> Option<Self> {
        Some(match cpp_type {
            "GLboolean" => Self::U8,
            "bool" => Self::Bool,
            "GLenum" | "GLuint" | "GLbitfield" | "WebGLObjectId" | "u32" => Self::U32,
            "GLint" | "GLsizei" | "int" => Self::I32,
            "GLfloat" => Self::F32,
            "GLintptr" | "GLsizeiptr" => Self::I64,
            "GLuint64" => Self::U64,
            "WebGLDataSpan" => Self::DataSpan,
            _ => return None,
        })
    }

    pub fn rust_type(self) -> &'static str {
        match self {
            Self::U8 => "u8",
            Self::Bool => "bool",
            Self::U32 => "u32",
            Self::I32 => "i32",
            Self::F32 => "f32",
            Self::I64 => "i64",
            Self::U64 => "u64",
            Self::DataSpan => "DataSpan",
        }
    }

    pub fn size(self) -> usize {
        match self {
            Self::U8 | Self::Bool => 1,
            Self::U32 | Self::I32 | Self::F32 => 4,
            Self::I64 | Self::U64 | Self::DataSpan => 8,
        }
    }

    pub fn alignment(self) -> usize {
        match self {
            Self::U8 | Self::Bool => 1,
            Self::U32 | Self::I32 | Self::F32 | Self::DataSpan => 4,
            Self::I64 | Self::U64 => 8,
        }
    }

    pub fn accepts_every_bit_pattern(self) -> bool {
        self != Self::Bool
    }
}

#[derive(Debug)]
pub struct WireField {
    pub spec: FieldSpec,
    pub wire_type: WireType,
    pub offset: usize,
}

#[derive(Debug)]
pub struct WireStruct {
    /// The name of the struct inside the C++ namespace Compositing::WebGL, which the table of layout words hashes.
    pub cpp_name: String,
    pub fields: Vec<WireField>,
    pub size: usize,
    pub alignment: usize,
}

impl WireStruct {
    /// The Rust struct has fields of its own for the bytes that are not part of a field of the model.
    pub fn has_unused_bytes(&self) -> bool {
        let size_of_fields: usize = self.fields.iter().map(|field| field.wire_type.size()).sum();
        size_of_fields != self.size
    }
}

pub fn wire_struct(cpp_name: String, field_specs: Vec<FieldSpec>) -> Result<WireStruct, String> {
    let mut fields = Vec::new();
    let mut end_of_fields: usize = 0;
    let mut alignment: usize = 1;
    for spec in field_specs {
        let Some(wire_type) = WireType::of(&spec.cpp_type) else {
            return Err(format!(
                "{cpp_name}.{} has the type {}, which the model of the WebGL functions has no layout for",
                spec.name, spec.cpp_type
            ));
        };
        let offset = end_of_fields.next_multiple_of(wire_type.alignment());
        end_of_fields = offset + wire_type.size();
        alignment = alignment.max(wire_type.alignment());
        fields.push(WireField {
            spec,
            wire_type,
            offset,
        });
    }
    // NB: A struct without fields has the size 1 in C++, and the replayer rejects a request that is shorter than
    //     its struct.
    let size = end_of_fields.next_multiple_of(alignment).max(1);
    Ok(WireStruct {
        cpp_name,
        fields,
        size,
        alignment,
    })
}

pub struct CommandEntry<'a> {
    pub function: &'a Function,
    pub layout: WireStruct,
}

pub struct SyncCallEntry<'a> {
    pub function: &'a Function,
    pub request: WireStruct,
    pub reply: WireStruct,
}

/// Everything that crosses the command stream, in the order of the command types and of the synchronous call types.
pub struct Vocabulary<'a> {
    pub commands: Vec<CommandEntry<'a>>,
    pub sync_calls: Vec<SyncCallEntry<'a>>,
}

pub fn vocabulary(functions: &[Function]) -> Result<Vocabulary<'_>, String> {
    let mut commands = Vec::new();
    let mut sync_calls = Vec::new();
    for function in functions {
        let name = function.command_name();
        if function.is_command_stream_entry() {
            let layout = wire_struct(format!("Commands::{name}"), function.command_fields()?)?;
            commands.push(CommandEntry { function, layout });
        }
        if function.is_sync_call() {
            let request = wire_struct(format!("SyncCalls::{name}::Request"), function.sync_request_fields()?)?;
            let reply = wire_struct(format!("SyncCalls::{name}::Reply"), function.sync_reply_fields())?;
            if let Some(field) = reply
                .fields
                .iter()
                .find(|field| !field.wire_type.accepts_every_bit_pattern())
            {
                return Err(format!(
                    "{}.{} is a {}, which cannot be read from the bytes of a reply",
                    reply.cpp_name, field.spec.name, field.spec.cpp_type
                ));
            }
            sync_calls.push(SyncCallEntry {
                function,
                request,
                reply,
            });
        }
    }
    Ok(Vocabulary { commands, sync_calls })
}

/// The 32-bit FNV-1a hash that stands for the name of a struct, a field or a type in the table of layout words.
pub fn wire_layout_name_hash(name: &str) -> u32 {
    name.bytes().fold(0x811C_9DC5, |hash, byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn a_name_hash_is_fnv_1a() {
        assert_eq!(wire_layout_name_hash(""), 0x811C_9DC5);
        assert_eq!(wire_layout_name_hash("a"), 0xE40C_292C);
        assert_eq!(wire_layout_name_hash("Commands::ActiveTexture"), 0x9B0F_0F7C);
    }
}
