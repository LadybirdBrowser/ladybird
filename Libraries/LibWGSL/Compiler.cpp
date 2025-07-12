/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NonnullRawPtr.h>
#include <AK/ScopeGuard.h>
#include <LibWGSL/Compiler.h>
#include <LibWGSL/Lexer.h>
#include <LibWGSL/Parser.h>
#include <LibWGSL/Preprocessor.h>

#include <spirv-tools/libspirv.h>
#include <spirv_cross/spirv_cross_c.h>

namespace WGSL {

namespace {

// https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_logical_layout_of_a_module
struct Operation {
    enum class Instruction : u8 {
        Invalid = 0,

        // Header
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpCapability
        Capability,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpMemoryModel
        MemoryModel,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpEntryPoint
        EntryPoint,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpExecutionMode
        ExecutionMode,

        // Debug
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpSource
        Source,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpDecorate
        Decorate,

        // Types
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpTypeVoid
        TypeVoid,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpTypeInt
        TypeInt,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpTypeFloat
        TypeFloat,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpTypeVector
        TypeVector,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpTypeStruct
        TypeStruct,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpTypeFunction
        TypeFunction,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpTypePointer
        TypePointer,

        // Variables
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpVariable
        Variable,

        // Constants
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpConstant
        Constant,

        // Functions
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpFunction
        Function,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpLabel
        Label,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpLoad
        Load,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpStore
        Store,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpAccessChain
        AccessChain,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpReturn
        Return,

        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpFunctionEnd
        FunctionEnd,
    };

    struct Identifier {
        u32 value { 0 };
    };

    Optional<Identifier> identifier {};
    Instruction instruction { Instruction::Invalid };
    Vector<Variant<Identifier, String, u32>> arguments;

    ErrorOr<String> text_assembly() const
    {
        StringBuilder text_asm_builder;
        if (identifier.has_value()) {
            text_asm_builder.appendff("%{} = "sv, identifier.value().value);
        }
        // FIXME: Using magic_enum would reduce a lot of boilerplate here
        switch (instruction) {
        case Instruction::Capability:
            text_asm_builder.append("OpCapability "sv);
            break;
        case Instruction::MemoryModel:
            text_asm_builder.append("OpMemoryModel "sv);
            break;
        case Instruction::EntryPoint:
            text_asm_builder.append("OpEntryPoint "sv);
            break;
        case Instruction::ExecutionMode:
            text_asm_builder.append("OpExecutionMode "sv);
            break;
        case Instruction::Source:
            text_asm_builder.append("OpSource "sv);
            break;
        case Instruction::Decorate:
            text_asm_builder.append("OpDecorate "sv);
            break;
        case Instruction::TypeVoid:
            text_asm_builder.append("OpTypeVoid "sv);
            break;
        case Instruction::TypeInt:
            text_asm_builder.append("OpTypeInt "sv);
            break;
        case Instruction::TypeFloat:
            text_asm_builder.append("OpTypeFloat "sv);
            break;
        case Instruction::TypeVector:
            text_asm_builder.append("OpTypeVector "sv);
            break;
        case Instruction::TypeStruct:
            text_asm_builder.append("OpTypeStruct "sv);
            break;
        case Instruction::TypeFunction:
            text_asm_builder.append("OpTypeFunction "sv);
            break;
        case Instruction::TypePointer:
            text_asm_builder.append("OpTypePointer "sv);
            break;
        case Instruction::Variable:
            text_asm_builder.append("OpVariable "sv);
            break;
        case Instruction::Constant:
            text_asm_builder.append("OpConstant "sv);
            break;
        case Instruction::Function:
            text_asm_builder.append("OpFunction "sv);
            break;
        case Instruction::Label:
            text_asm_builder.append("OpLabel "sv);
            break;
        case Instruction::Load:
            text_asm_builder.append("OpLoad "sv);
            break;
        case Instruction::Store:
            text_asm_builder.append("OpStore "sv);
            break;
        case Instruction::AccessChain:
            text_asm_builder.append("OpAccessChain "sv);
            break;
        case Instruction::Return:
            text_asm_builder.append("OpReturn "sv);
            break;
        case Instruction::FunctionEnd:
            text_asm_builder.append("OpFunctionEnd"sv);
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        for (auto const& argument : arguments) {
            argument.visit([&text_asm_builder](Identifier const& id) { text_asm_builder.appendff("%{} "sv, id.value); },
                [&text_asm_builder](String const& str) {
                    text_asm_builder.appendff("{} "sv, str);
                },
                [&text_asm_builder](u32 const literal) {
                    text_asm_builder.appendff("{} "sv, literal);
                });
        }

        auto const text_asm = TRY(text_asm_builder.to_string());
        return TRY(text_asm.trim_whitespace());
    }

    static Identifier next_id()
    {
        static u32 id { 0 };
        return Identifier { .value = ++id };
    }
};

void spv_message_consumer_callback(spv_message_level_t const level, char const* source,
    spv_position_t const* position, char const* message)
{
    if (level == SPV_MSG_ERROR || level == SPV_MSG_FATAL) {
        dbgln("SPIR-V Error: {}"sv, message);
        if (source)
            dbgln("\tsource {}"sv, source);
        if (position)
            dbgln("\tposition {}:{}"sv, position->line, position->column);
    } else if (level == SPV_MSG_WARNING) {
        warnln("SPIR-V Warning: {}"sv, message);
    }
}

} // unnamed namespace

Compiler::Compiler(StringView const source)
    : m_source(source)
{
}

ErrorOr<String> Compiler::emit_spirv_text()
{
    auto [declarations] = TRY(parse());

    Vector<Operation> header_operations;
    header_operations.append(Operation { .instruction = Operation::Instruction::Capability, .arguments = { "Shader"_string } });
    header_operations.append(Operation { .instruction = Operation::Instruction::MemoryModel, .arguments = { "Logical"_string, "GLSL450"_string } });

    Vector<Operation> debug_operations;
    debug_operations.append(Operation { .instruction = Operation::Instruction::Source, .arguments = { "WGSL"_string, "100"_string } });

    Vector<NonnullRawPtr<StructDeclaration const>> struct_decls;
    Vector<NonnullRawPtr<FunctionDeclaration const>> functions_decls;

    Vector<Operation> type_operations;
    auto const op_type_void_id = Operation::next_id();
    type_operations.append(Operation { .identifier = op_type_void_id, .instruction = Operation::Instruction::TypeVoid, .arguments = {} });

    auto const op_type_float_id = Operation::next_id();
    type_operations.append(Operation { .identifier = op_type_float_id, .instruction = Operation::Instruction::TypeFloat, .arguments = { 32 } });

    auto const op_type_int_id = Operation::next_id();
    type_operations.append(Operation { .identifier = op_type_int_id, .instruction = Operation::Instruction::TypeInt, .arguments = { 32, 1 } });

    struct Variable {
        Operation::Identifier type_id;
        Operation::Identifier variable_id;
        String storage_class;
        // FIXME: Create Variant for member variables and regular variables
        Optional<String> member_name;

        // FIXME: Find nicer way to mark variables as loaded. All OpVariable instructions (for function local variables) have to be declared before and OpLoad calls so some extra organization is reuqired here
        Optional<Operation::Identifier> load_id;
    };

    struct Member {
        u32 idx { 0 };
        Variable variable;
        Vector<NonnullRefPtr<Attribute>> attributes;
    };
    HashMap<StringView, HashMap<StringView, Member>> struct_member_lookup;
    HashMap<VectorType::Kind, Operation::Identifier> vector_id_lookup;

    using Data = Variant<Variable, NonnullRawPtr<HashMap<StringView, Member>>>;

    auto get_data = [&struct_member_lookup, &vector_id_lookup](NonnullRefPtr<Type> const& type) -> ErrorOr<Data> {
        if (auto const* named_type = as_if<NamedType>(*type)) {
            if (auto struct_members_itr = struct_member_lookup.find(named_type->name()); struct_members_itr != struct_member_lookup.end()) {
                return Data { struct_members_itr->value };
            }
        } else if (auto const* vector_type = as_if<VectorType>(*type)) {
            if (auto vector_id_itr = vector_id_lookup.find(vector_type->kind()); vector_id_itr != vector_id_lookup.end()) {
                Variable vector_variable;
                vector_variable.type_id = vector_id_itr->value;
                return Data { vector_variable };
            }
        }
        return Error::from_string_literal("Unknown type");
    };

    for (auto const& decl : declarations) {
        if (auto const* struct_decl = as_if<StructDeclaration>(*decl)) {
            struct_decls.append(*struct_decl);
        } else if (auto const* func_decl = as_if<FunctionDeclaration>(*decl)) {
            functions_decls.append(*func_decl);
        }
    }

    // NOTE: Decoration validation rules state that if a struct member has a built-in decoration via OpDecorateMember, all members of the struct need the built-in.
    // The current WGSL spec allows mixing and matching built-in attributes with non-built-in attributes. Due to this, we don't use OpTypeStruct's and just split each
    // member into a standalone variable. This allows us to use OpDecorate and avoid the mixed decorations rule
    // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_universal_validation_rules

    auto decorate_member = [&debug_operations](Member const& member) {
        for (auto const& member_attribute : member.attributes) {
            if (auto const* location_attribute = as_if<LocationAttribute>(*member_attribute)) {
                debug_operations.append(Operation { .instruction = Operation::Instruction::Decorate, .arguments = { member.variable.variable_id, "Location"_string, location_attribute->value() } });
            } else if (auto const* builtin_attribute = as_if<BuiltinAttribute>(*member_attribute)) {
                Operation decorate_op;
                decorate_op.instruction = Operation::Instruction::Decorate;
                decorate_op.arguments.append(member.variable.variable_id);
                decorate_op.arguments.append("BuiltIn"_string);
                switch (builtin_attribute->kind()) {
                case BuiltinAttribute::Kind::Position:
                    decorate_op.arguments.append("Position"_string);
                    break;
                }
                debug_operations.append(decorate_op);
            }
        }
    };

    for (auto const& decl : struct_decls) {
        Vector<Operation::Identifier> member_ids;
        auto const& members = decl->members();
        for (auto const& member : members) {
            auto const& type = member->type();
            if (auto const* vector_type = as_if<VectorType>(*type)) {
                auto kind = vector_type->kind();
                if (auto vector_id_itr = vector_id_lookup.find(kind); vector_id_itr != vector_id_lookup.end()) {
                    member_ids.append(vector_id_itr->value);
                } else {
                    auto const member_id = Operation::next_id();
                    member_ids.append(member_id);
                    Operation member_type_op;
                    member_type_op.identifier = member_id;
                    member_type_op.instruction = Operation::Instruction::TypeVector;
                    member_type_op.arguments.append(op_type_float_id);

                    switch (kind) {
                    case VectorType::Kind::Vec3f:
                        member_type_op.arguments.append(3);
                        break;
                    case VectorType::Kind::Vec4f:
                        member_type_op.arguments.append(4);
                        break;
                    }

                    type_operations.append(member_type_op);
                    vector_id_lookup.set(kind, member_id);
                }
            }
        }

        HashMap<StringView, Member> member_lookup;

        for (u32 idx = 0; idx < members.size(); ++idx) {
            auto const& member = members[idx];
            auto member_data = TRY(get_data(member->type()));
            auto* member_data_val = member_data.get_pointer<Variable>();
            VERIFY(member_data_val != nullptr);
            member_data_val->member_name = member->name();
            member_lookup.set(member->name(), Member { idx, *member_data_val, member->attributes() });
        }

        struct_member_lookup.set(decl->name(), member_lookup);
    }

    Operation func_type_op;
    auto const func_type_op_id = Operation::next_id();
    func_type_op.identifier = func_type_op_id;
    func_type_op.instruction = Operation::Instruction::TypeFunction;
    func_type_op.arguments.append(op_type_void_id);
    type_operations.append(func_type_op);

    Vector<Operation> function_operations;
    for (auto const& decl : functions_decls) {
        auto const& return_type = decl->return_type();
        // FIXME: return_type() should not be optional
        if (!return_type.has_value()) {
            return Error::from_string_literal("Missing return type");
        }
        auto return_data = TRY(get_data(return_type.value()));

        Vector<Operation::Identifier> entry_point_variable_ids;

        HashMap<StringView, Vector<Variable>> input_variables;
        HashMap<StringView, Vector<Variable>> local_variables;

        auto get_variables = [&input_variables, &local_variables](String const& name) -> ErrorOr<Vector<Variable>> {
            auto variable_itr = input_variables.find(name);
            if (variable_itr != input_variables.end()) {
                return variable_itr->value;
            }
            variable_itr = local_variables.find(name);
            if (variable_itr != local_variables.end()) {
                return variable_itr->value;
            }
            return Error::from_string_literal("Unknown variables");
        };

        for (auto const& param : decl->parameters()) {
            auto const& name = param->name();
            auto const& type = param->type();
            auto data = TRY(get_data(type));

            data.visit(
                [&input_variables, &name, &type_operations, &entry_point_variable_ids](Variable& variable) {
                    auto const input_pointer_id = Operation::next_id();
                    type_operations.append(Operation { .identifier = input_pointer_id, .instruction = Operation::Instruction::TypePointer, .arguments = { "Input"_string, variable.type_id } });

                    auto const input_variable_id = Operation::next_id();
                    type_operations.append(Operation { .identifier = input_variable_id, .instruction = Operation::Instruction::Variable, .arguments = { input_pointer_id, "Input"_string } });
                    entry_point_variable_ids.append(input_variable_id);
                    variable.variable_id = input_variable_id;
                    variable.storage_class = "Input"_string;
                    input_variables.set(name, Vector { variable });
                },
                [&input_variables, &name, &type_operations, &entry_point_variable_ids](NonnullRawPtr<HashMap<StringView, Member>>& members) {
                    Vector<Variable> member_variables;
                    for (auto& [_, value] : *members) {
                        auto& member_variable = value.variable;
                        auto const input_pointer_id = Operation::next_id();
                        type_operations.append(Operation { .identifier = input_pointer_id, .instruction = Operation::Instruction::TypePointer, .arguments = { "Input"_string, member_variable.type_id } });

                        auto const input_variable_id = Operation::next_id();
                        type_operations.append(Operation { .identifier = input_variable_id, .instruction = Operation::Instruction::Variable, .arguments = { input_pointer_id, "Input"_string } });
                        member_variable.variable_id = input_variable_id;
                        member_variable.storage_class = "Input"_string;
                        entry_point_variable_ids.append(input_variable_id);
                        member_variables.append(member_variable);
                    }
                    input_variables.set(name, member_variables);
                });
        }

        Vector<Operation::Identifier> output_variable_ids;
        return_data.visit(
            [&type_operations, &output_variable_ids, &entry_point_variable_ids](Variable& variable) {
                auto const output_pointer_id = Operation::next_id();
                type_operations.append(Operation { .identifier = output_pointer_id, .instruction = Operation::Instruction::TypePointer, .arguments = { "Output"_string, variable.type_id } });

                auto const output_variable_id = Operation::next_id();
                type_operations.append(Operation { .identifier = output_variable_id, .instruction = Operation::Instruction::Variable, .arguments = { output_pointer_id, "Output"_string } });
                variable.variable_id = output_variable_id;
                variable.storage_class = "Output"_string;
                output_variable_ids.append(output_variable_id);
                entry_point_variable_ids.append(output_variable_id);
            },
            [&type_operations, &output_variable_ids, &entry_point_variable_ids, &decorate_member](NonnullRawPtr<HashMap<StringView, Member>>& members) {
                for (auto& [_, member] : *members) {
                    auto& member_variable = member.variable;
                    auto const output_pointer_id = Operation::next_id();
                    type_operations.append(Operation { .identifier = output_pointer_id, .instruction = Operation::Instruction::TypePointer, .arguments = { "Output"_string, member_variable.type_id } });

                    auto const output_variable_id = Operation::next_id();
                    type_operations.append(Operation { .identifier = output_variable_id, .instruction = Operation::Instruction::Variable, .arguments = { output_pointer_id, "Output"_string } });
                    member_variable.variable_id = output_variable_id;
                    member_variable.storage_class = "Output"_string;
                    output_variable_ids.append(output_variable_id);
                    entry_point_variable_ids.append(output_variable_id);
                    decorate_member(member);
                }
            });

        Operation func_op;
        auto const func_op_id = Operation::next_id();
        func_op.identifier = func_op_id;
        func_op.instruction = Operation::Instruction::Function;
        func_op.arguments.append(op_type_void_id);
        // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#Function_Control
        func_op.arguments.append("None"_string);
        func_op.arguments.append(func_type_op_id);
        function_operations.append(func_op);

        function_operations.append(Operation { .identifier = Operation::next_id(), .instruction = Operation::Instruction::Label, .arguments = {} });

        Vector<NonnullRawPtr<VariableStatement const>> variable_statements;
        Vector<NonnullRawPtr<Statement const>> statements;
        for (auto const& statement : decl->body()) {
            if (auto const* variable_statement = as_if<VariableStatement>(*statement)) {
                variable_statements.append(*variable_statement);
            } else {
                statements.append(*statement);
            }
        }

        for (auto const& variable_statement : variable_statements) {
            auto const& var_name = variable_statement->name();
            auto const& var_type = variable_statement->type();
            // FIXME: type() should not be optional
            if (!var_type.has_value()) {
                return Error::from_string_literal("Missing variable type");
            }
            auto var_data = TRY(get_data(var_type.value()));

            var_data.visit(
                [&local_variables, &var_name, &type_operations, &function_operations](Variable& variable) {
                    auto const var_pointer_id = Operation::next_id();
                    type_operations.append(Operation { .identifier = var_pointer_id, .instruction = Operation::Instruction::TypePointer, .arguments = { "Function"_string, variable.type_id } });

                    auto const var_variable_id = Operation::next_id();
                    function_operations.append(Operation { .identifier = var_variable_id, .instruction = Operation::Instruction::Variable, .arguments = { var_pointer_id, "Function"_string } });
                    variable.variable_id = var_variable_id;
                    variable.storage_class = "Function"_string;

                    local_variables.set(var_name, Vector { variable });
                },
                [&local_variables, &var_name, &type_operations, &function_operations](NonnullRawPtr<HashMap<StringView, Member>>& members) {
                    Vector<Variable> member_variables;
                    for (auto& [_, value] : *members) {
                        auto& member_variable = value.variable;
                        auto const var_pointer_id = Operation::next_id();
                        type_operations.append(Operation { .identifier = var_pointer_id, .instruction = Operation::Instruction::TypePointer, .arguments = { "Function"_string, member_variable.type_id } });

                        auto const var_variable_id = Operation::next_id();
                        function_operations.append(Operation { .identifier = var_variable_id, .instruction = Operation::Instruction::Variable, .arguments = { var_pointer_id, "Function"_string } });
                        member_variable.variable_id = var_variable_id;
                        member_variable.storage_class = "Function"_string;
                        member_variables.append(member_variable);
                    }
                    local_variables.set(var_name, member_variables);
                });
        }

        for (auto& [_, value] : input_variables) {
            for (auto& input_variable : value) {
                auto const input_id = Operation::next_id();
                function_operations.append(Operation { .identifier = input_id, .instruction = Operation::Instruction::Load, .arguments = { input_variable.type_id, input_variable.variable_id } });
                input_variable.load_id = input_id;
            }
        }

        auto access_member_variable = [/*&type_operations, &function_operations, &op_type_int_id, &struct_member_lookup*/ &get_variables](MemberAccessExpression const& member_access_expr) -> ErrorOr<Variable> {
            auto const& object_expr = member_access_expr.object();
            auto const& member_name = member_access_expr.member();
            // FIXME: Handle recursive member access expressions, stop case is the first identifier expression
            if (auto const* object_identifier_expr = as_if<IdentifierExpression>(*object_expr)) {
                auto const& object_name = object_identifier_expr->name();
                for (auto const object_member_variables = TRY(get_variables(object_name)); auto const& object_member_variable : object_member_variables) {
                    auto const& object_member_name = object_member_variable.member_name;
                    if (!object_member_name.has_value())
                        continue;
                    if (object_member_name.value() == member_name)
                        return object_member_variable;
                }
                return Error::from_string_literal("Unknown object member variable");
            }
            return Error::from_string_literal("Unsupported object expression");
        };

        for (auto const& statement : statements) {
            if (auto const* assign_statement = as_if<AssignmentStatement>(*statement)) {
                auto const& rhs_expr = assign_statement->rhs();
                Variable rhs_variable;
                if (auto const* member_access_expr = as_if<MemberAccessExpression>(*rhs_expr)) {
                    rhs_variable = TRY(access_member_variable(*member_access_expr));
                }

                if (!rhs_variable.load_id.has_value())
                    return Error::from_string_literal("Member variable is not loaded");

                auto const& lhs_expr = assign_statement->lhs();
                Variable lhs_variable;
                if (auto const* member_access_expr = as_if<MemberAccessExpression>(*lhs_expr)) {
                    lhs_variable = TRY(access_member_variable(*member_access_expr));
                }
                function_operations.append(Operation { .instruction = Operation::Instruction::Store, .arguments = { lhs_variable.variable_id, rhs_variable.load_id.value() } });
            } else if (auto const* return_statement = as_if<ReturnStatement>(*statement)) {
                if (auto const& return_expr = return_statement->expression(); return_expr.has_value()) {
                    auto const& return_expression = return_expr.value();
                    if (auto const* identifier_expr = as_if<IdentifierExpression>(*return_expression)) {
                        auto identifier_variables = TRY(get_variables(identifier_expr->name()));
                        Vector<Operation::Identifier> identifier_load_ids;
                        for (auto const& variable : identifier_variables) {
                            auto const identifier_load_id = Operation::next_id();
                            function_operations.append(Operation { .identifier = identifier_load_id, .instruction = Operation::Instruction::Load, .arguments = { variable.type_id, variable.variable_id } });
                            identifier_load_ids.append(identifier_load_id);
                        }
                        if (output_variable_ids.size() != identifier_load_ids.size()) {
                            return Error::from_string_literal("Output variables size does not match the return variables size");
                        }
                        for (size_t i = 0; i < output_variable_ids.size(); ++i) {
                            auto const output_variable_id = output_variable_ids[i];
                            auto const load_identifier_id = identifier_load_ids[i];
                            function_operations.append(Operation { .instruction = Operation::Instruction::Store, .arguments = { output_variable_id, load_identifier_id } });
                        }
                    } else if (auto const* member_access_expr = as_if<MemberAccessExpression>(*return_expression)) {
                        auto const member_variable = TRY(access_member_variable(*member_access_expr));
                        if (!member_variable.load_id.has_value())
                            return Error::from_string_literal("Member variable is not loaded");
                        if (output_variable_ids.size() != 1)
                            return Error::from_string_literal("Expected single output variable");
                        function_operations.append(Operation { .instruction = Operation::Instruction::Store, .arguments = { output_variable_ids.first(), member_variable.load_id.value() } });
                    }
                }
                function_operations.append(Operation { .instruction = Operation::Instruction::Return, .arguments = {} });
            }
        }

        function_operations.append(Operation { .instruction = Operation::Instruction::FunctionEnd, .arguments = {} });

        Operation entry_point_op;
        entry_point_op.instruction = Operation::Instruction::EntryPoint;
        String entry_point_exec_model;
        StringBuilder entry_point_name_builder;

        Optional<Operation> exec_mode_op;

        for (auto const& attr : decl->attributes()) {
            if (is<VertexAttribute>(*attr)) {
                entry_point_exec_model = "Vertex"_string;
                entry_point_name_builder.appendff(R"("{}")", decl->name());
                break;
            } else if (is<FragmentAttribute>(*attr)) {
                entry_point_exec_model = "Fragment"_string;
                entry_point_name_builder.appendff(R"("{}")", decl->name());
                // https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#Execution_Mode
                exec_mode_op = Operation { .instruction = Operation::Instruction::ExecutionMode, .arguments = { func_op_id, "OriginLowerLeft"_string } };
                break;
            }
        }

        entry_point_op.arguments.append(entry_point_exec_model);
        entry_point_op.arguments.append(func_op_id);
        entry_point_op.arguments.append(TRY(entry_point_name_builder.to_string()));
        for (auto const& variable_id : entry_point_variable_ids) {
            entry_point_op.arguments.append(variable_id);
        }

        header_operations.append(entry_point_op);
        if (exec_mode_op.has_value()) {
            header_operations.append(exec_mode_op.value());
        }

        for (auto const& return_attribute : decl->return_attributes()) {
            if (auto const* location_attribute = as_if<LocationAttribute>(*return_attribute)) {
                return_data.visit(
                    [&debug_operations, &location_attribute](Variable const& variable) {
                        debug_operations.append(Operation { .instruction = Operation::Instruction::Decorate, .arguments = { variable.variable_id, "Location"_string, location_attribute->value() } });
                    },
                    [&debug_operations, &location_attribute](HashMap<StringView, Member> const& members) {
                        for (auto const& [_, value] : members) {
                            auto const& member_variable = value.variable;
                            debug_operations.append(Operation { .instruction = Operation::Instruction::Decorate, .arguments = { member_variable.variable_id, "Location"_string, location_attribute->value() } });
                        }
                    });
            }
            // FIXME: Support all relevant attributes
        }
    }

    for (auto const& [_, members] : struct_member_lookup) {
        for (auto const& [_, member] : members) {
            decorate_member(member);
        }
    }

    Vector<String> text_assembly;
    text_assembly.append("; Magic:     0x07230203 (SPIR-V)"_string);
    text_assembly.append("; Version:   0x00010600 (Version: 1.6.0)"_string);
    // NOTE: Current official SPIRV-V tool IDs that we should avoid
    // https://github.com/KhronosGroup/SPIRV-Headers/blob/main/include/spirv/spir-v.xml
    // FIXME: Should Ladybird's WGSL compiler eventually be submitted as an official SPIR-V tool?
    text_assembly.append("; Generator: 0xFFFF0001 (Ladybird LibWGSL; 1)"_string);
    text_assembly.append("; Bound:     100"_string);
    text_assembly.append("; Schema:    0"_string);

    for (auto const& op : header_operations) {
        text_assembly.append(TRY(op.text_assembly()));
    }
    for (auto const& op : debug_operations) {
        text_assembly.append(TRY(op.text_assembly()));
    }
    for (auto const& op : type_operations) {
        text_assembly.append(TRY(op.text_assembly()));
    }
    for (auto const& op : function_operations) {
        text_assembly.append(TRY(op.text_assembly()));
    }

    StringBuilder spirv_builder;
    spirv_builder.join("\n"sv, text_assembly);
    return TRY(spirv_builder.to_string());
}

ErrorOr<Vector<uint32_t>> Compiler::emit_spirv_binary(StringView const text_assembly)
{
    ByteString const byte_string_assembly = text_assembly.to_byte_string();

    spv_context context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    ScopeGuard const context_guard([&context] { spvContextDestroy(context); });
    if (!context) {
        return Error::from_string_literal("Unable to create SPIR-V context");
    }

    spv_binary binary = nullptr;
    ScopeGuard const binary_guard([&binary] { spvBinaryDestroy(binary); });

    spv_diagnostic diagnostic = nullptr;
    ScopeGuard const diagnostic_guard([&diagnostic] {
        spvDiagnosticDestroy(diagnostic);
        diagnostic = nullptr;
    });

    spv_result_t const assemble_result = spvTextToBinaryWithOptions(
        context,
        byte_string_assembly.characters(),
        byte_string_assembly.length(),
        SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS,
        &binary,
        &diagnostic);

    if (assemble_result != SPV_SUCCESS) {
        dbgln("SPIR-V assembly error: {}", diagnostic->error);
        return Error::from_string_literal("Unable to assemble SPIR-V text");
    }

    spv_optimizer_t* optimizer = spvOptimizerCreate(SPV_ENV_UNIVERSAL_1_6);
    ScopeGuard const optimizer_guard([&optimizer] { spvOptimizerDestroy(optimizer); });
    if (!optimizer) {
        return Error::from_string_literal("Unable to create SPIR-V optimizer");
    }
    spvOptimizerSetMessageConsumer(optimizer, spv_message_consumer_callback);
    spvOptimizerRegisterPerformancePasses(optimizer);

    spv_optimizer_options optimizer_options = spvOptimizerOptionsCreate();
    ScopeGuard const optimizer_options_guard([&optimizer_options] { spvOptimizerOptionsDestroy(optimizer_options); });

    spvOptimizerOptionsSetRunValidator(optimizer_options, true);

    spv_validator_options validator_options = spvValidatorOptionsCreate();
    ScopeGuard const validator_options_guard([&validator_options] { spvValidatorOptionsDestroy(validator_options); });

    spvValidatorOptionsSetRelaxStoreStruct(validator_options, true);
    spvValidatorOptionsSetSkipBlockLayout(validator_options, true);

    spvOptimizerOptionsSetValidatorOptions(optimizer_options, validator_options);

    spv_binary optimized_binary = nullptr;
    ScopeGuard const optimized_binary_guard([&optimized_binary] { spvBinaryDestroy(optimized_binary); });
    spv_result_t const optimize_result = spvOptimizerRun(
        optimizer,
        binary->code,
        binary->wordCount,
        &optimized_binary,
        optimizer_options);

    if (optimize_result != SPV_SUCCESS) {
        return Error::from_string_literal("Failed to optimize SPIR-V binary");
    }

    if (optimized_binary && optimized_binary->wordCount > 0) {
        return Vector<uint32_t> { ReadonlySpan<uint32_t> { optimized_binary->code, optimized_binary->wordCount } };
    }
    return Vector<uint32_t> { ReadonlySpan<uint32_t> { binary->code, binary->wordCount } };
}

static ErrorOr<Vector<Compiler::Shader>> emit_backend(spvc_backend const backend, Vector<uint32_t> const& spirv)
{
    spvc_context context = { nullptr };
    spvc_result result = spvc_context_create(&context);
    if (result != SPVC_SUCCESS)
        return Error::from_string_literal("Unable to create SPIR-V context");
    ScopeGuard const context_guard([&context] { spvc_context_destroy(context); });

    spvc_parsed_ir ir = { nullptr };
    result = spvc_context_parse_spirv(context, spirv.data(), spirv.size(), &ir);
    if (result != SPVC_SUCCESS)
        return Error::from_string_literal("Unable to parse SPIR-V source");

    spvc_compiler compiler = { nullptr };
    result = spvc_context_create_compiler(context, backend, ir,
        SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler);
    if (result != SPVC_SUCCESS)
        return Error::from_string_literal("Unable to create backend compiler");

    Vector<Compiler::Shader> shader_sources;

    spvc_entry_point const* entry_points = { nullptr };
    size_t num_entry_points = 0;
    result = spvc_compiler_get_entry_points(compiler, &entry_points, &num_entry_points);
    if (result != SPVC_SUCCESS)
        return Error::from_string_literal("Unable to determine entry points in the SPIR-V source");

    for (size_t i = 0; i < num_entry_points; ++i) {
        spvc_entry_point const* entry_point = &entry_points[i];

        result = spvc_compiler_set_entry_point(compiler, entry_point->name, entry_point->execution_model);
        if (result != SPVC_SUCCESS)
            return Error::from_string_literal("Unable to set compiler entry point");

        spvc_compiler_options options = { nullptr };
        result = spvc_compiler_create_compiler_options(compiler, &options);
        if (result != SPVC_SUCCESS)
            return Error::from_string_literal("Unable to create compiler options");

        result = spvc_compiler_install_compiler_options(compiler, options);
        if (result != SPVC_SUCCESS)
            return Error::from_string_literal("Unable to install compiler options");

        char const* backend_source_data = { nullptr };
        result = spvc_compiler_compile(compiler, &backend_source_data);
        if (result != SPVC_SUCCESS)
            return Error::from_string_literal("Unable to cross compile SPIR-V into the backend source code");
        auto const backend_source = TRY(String::from_byte_string(ByteString { backend_source_data, strlen(backend_source_data) }));
        auto const entry_point_name = TRY(String::from_byte_string(ByteString { entry_point->name, strlen(entry_point->name) }));
        Compiler::Shader const shader_source = TRY([&entry_point, &entry_point_name, &backend_source] -> ErrorOr<Compiler::Shader> {
            switch (entry_point->execution_model) {
            case SpvExecutionModelVertex:
                return Compiler::VertexShader { entry_point_name, backend_source };
            case SpvExecutionModelFragment:
                return Compiler::FragmentShader { entry_point_name, backend_source };
            default:
                return Error::from_string_literal("Unsupported entry point execution model");
            }
        }());
        shader_sources.append(shader_source);
    }
    return shader_sources;
}

ErrorOr<Vector<Compiler::Shader>> Compiler::emit_msl()
{
    Vector<uint32_t> const spirv = TRY(emit_spirv_binary(TRY(emit_spirv_text())));
    return emit_backend(SPVC_BACKEND_MSL, spirv);
}

ErrorOr<Program> Compiler::parse()
{
    Preprocessor preprocessor(m_source);
    auto const processed_text = TRY(preprocessor.process());
    Lexer lexer(processed_text);
    Vector<Token> tokens;
    while (true) {
        auto token = lexer.next_token();
        tokens.append(token);
        if (token.type.has<EndOfFileToken>())
            break;
    }

    Parser parser(move(tokens));
    return TRY(parser.parse());
}

}
