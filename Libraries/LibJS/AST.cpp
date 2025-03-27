/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/TemporaryChange.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/RootVector.h>
#include <LibJS/AST.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <typeinfo>

namespace JS {

ASTNode::ASTNode(SourceRange source_range)
    : m_start_offset(source_range.start.offset)
    , m_source_code(source_range.code)
    , m_end_offset(source_range.end.offset)
{
}

SourceRange ASTNode::source_range() const
{
    return m_source_code->range_from_offsets(m_start_offset, m_end_offset);
}

ByteString ASTNode::class_name() const
{
    // NOTE: We strip the "JS::" prefix.
    auto const* typename_ptr = typeid(*this).name();
    return demangle({ typename_ptr, strlen(typename_ptr) }).substring(4);
}

static void print_indent(int indent)
{
    out("{}", ByteString::repeated(' ', indent * 2));
}

static void update_function_name(Value value, FlyString const& name)
{
    if (!value.is_function())
        return;
    auto& function = value.as_function();
    if (is<ECMAScriptFunctionObject>(function) && function.name().is_empty())
        static_cast<ECMAScriptFunctionObject&>(function).set_name(name);
}

void LabelledStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent + 1);
    outln("(Label)");
    print_indent(indent + 2);
    outln("\"{}\"", m_label);

    print_indent(indent + 1);
    outln("(Labelled item)");
    m_labelled_item->dump(indent + 2);
}

// 15.2.5 Runtime Semantics: InstantiateOrdinaryFunctionExpression, https://tc39.es/ecma262/#sec-runtime-semantics-instantiateordinaryfunctionexpression
Value FunctionExpression::instantiate_ordinary_function_expression(VM& vm, FlyString given_name) const
{
    auto& realm = *vm.current_realm();

    if (given_name.is_empty())
        given_name = ""_fly_string;
    auto has_own_name = !name().is_empty();

    auto const used_name = has_own_name ? name() : given_name;
    auto environment = GC::Ref { *vm.running_execution_context().lexical_environment };
    if (has_own_name) {
        VERIFY(environment);
        environment = new_declarative_environment(*environment);
        MUST(environment->create_immutable_binding(vm, name(), false));
    }

    auto private_environment = vm.running_execution_context().private_environment;

    auto closure = ECMAScriptFunctionObject::create(realm, used_name, source_text(), body(), parameters(), function_length(), local_variables_names(), environment, private_environment, kind(), is_strict_mode(),
        parsing_insights(), is_arrow_function());

    // FIXME: 6. Perform SetFunctionName(closure, name).
    // FIXME: 7. Perform MakeConstructor(closure).

    if (has_own_name)
        MUST(environment->initialize_binding(vm, name(), closure, Environment::InitializeBindingHint::Normal));

    return closure;
}

Optional<String> CallExpression::expression_string() const
{
    if (is<Identifier>(*m_callee))
        return static_cast<Identifier const&>(*m_callee).string().to_string();

    if (is<MemberExpression>(*m_callee))
        return static_cast<MemberExpression const&>(*m_callee).to_string_approximation();

    return {};
}

static ThrowCompletionOr<ClassElementName> class_key_to_property_name(VM& vm, Expression const& key, Value prop_key)
{
    if (is<PrivateIdentifier>(key)) {
        auto& private_identifier = static_cast<PrivateIdentifier const&>(key);
        auto private_environment = vm.running_execution_context().private_environment;
        VERIFY(private_environment);
        return ClassElementName { private_environment->resolve_private_identifier(private_identifier.string()) };
    }

    VERIFY(!prop_key.is_empty());

    if (prop_key.is_object())
        prop_key = TRY(prop_key.to_primitive(vm, Value::PreferredType::String));

    auto property_key = TRY(PropertyKey::from_value(vm, prop_key));
    return ClassElementName { property_key };
}

// 15.4.5 Runtime Semantics: MethodDefinitionEvaluation, https://tc39.es/ecma262/#sec-runtime-semantics-methoddefinitionevaluation
ThrowCompletionOr<ClassElement::ClassValue> ClassMethod::class_element_evaluation(VM& vm, Object& target, Value property_key) const
{
    auto property_key_or_private_name = TRY(class_key_to_property_name(vm, *m_key, property_key));

    auto& method_function = *ECMAScriptFunctionObject::create(*vm.current_realm(), m_function->name(), m_function->source_text(), m_function->body(), m_function->parameters(), m_function->function_length(), m_function->local_variables_names(), vm.lexical_environment(), vm.running_execution_context().private_environment, m_function->kind(), m_function->is_strict_mode(),
        m_function->parsing_insights(), m_function->is_arrow_function());

    auto method_value = Value(&method_function);
    method_function.make_method(target);

    auto set_function_name = [&](ByteString prefix = "") {
        auto name = property_key_or_private_name.visit(
            [&](PropertyKey const& property_key) -> String {
                if (property_key.is_symbol()) {
                    auto description = property_key.as_symbol()->description();
                    if (!description.has_value() || description->is_empty())
                        return ""_string;
                    return MUST(String::formatted("[{}]", *description));
                }
                return property_key.to_string();
            },
            [&](PrivateName const& private_name) -> String {
                return private_name.description.to_string();
            });

        update_function_name(method_value, MUST(String::formatted("{}{}{}", prefix, prefix.is_empty() ? "" : " ", name)));
    };

    if (property_key_or_private_name.has<PropertyKey>()) {
        auto& property_key = property_key_or_private_name.get<PropertyKey>();
        switch (kind()) {
        case ClassMethod::Kind::Method:
            set_function_name();
            TRY(target.define_property_or_throw(property_key, { .value = method_value, .writable = true, .enumerable = false, .configurable = true }));
            break;
        case ClassMethod::Kind::Getter:
            set_function_name("get");
            TRY(target.define_property_or_throw(property_key, { .get = &method_function, .enumerable = true, .configurable = true }));
            break;
        case ClassMethod::Kind::Setter:
            set_function_name("set");
            TRY(target.define_property_or_throw(property_key, { .set = &method_function, .enumerable = true, .configurable = true }));
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        return ClassValue { normal_completion({}) };
    } else {
        auto& private_name = property_key_or_private_name.get<PrivateName>();
        switch (kind()) {
        case Kind::Method:
            set_function_name();
            return ClassValue { PrivateElement { private_name, PrivateElement::Kind::Method, method_value } };
        case Kind::Getter:
            set_function_name("get");
            return ClassValue { PrivateElement { private_name, PrivateElement::Kind::Accessor, Value(Accessor::create(vm, &method_function, nullptr)) } };
        case Kind::Setter:
            set_function_name("set");
            return ClassValue { PrivateElement { private_name, PrivateElement::Kind::Accessor, Value(Accessor::create(vm, nullptr, &method_function)) } };
        default:
            VERIFY_NOT_REACHED();
        }
    }
}

void ClassFieldInitializerStatement::dump(int) const
{
    // This should not be dumped as it is never part of an actual AST.
    VERIFY_NOT_REACHED();
}

// 15.7.10 Runtime Semantics: ClassFieldDefinitionEvaluation, https://tc39.es/ecma262/#sec-runtime-semantics-classfielddefinitionevaluation
ThrowCompletionOr<ClassElement::ClassValue> ClassField::class_element_evaluation(VM& vm, Object& target, Value property_key) const
{
    auto& realm = *vm.current_realm();

    auto property_key_or_private_name = TRY(class_key_to_property_name(vm, *m_key, property_key));
    GC::Ptr<ECMAScriptFunctionObject> initializer;
    if (m_initializer) {
        auto copy_initializer = m_initializer;
        auto name = property_key_or_private_name.visit(
            [&](PropertyKey const& property_key) -> String {
                return property_key.is_number() ? property_key.to_string() : property_key.to_string_or_symbol().to_display_string();
            },
            [&](PrivateName const& private_name) -> String {
                return private_name.description.to_string();
            });

        // FIXME: A potential optimization is not creating the functions here since these are never directly accessible.
        auto function_code = create_ast_node<ClassFieldInitializerStatement>(m_initializer->source_range(), copy_initializer.release_nonnull(), name);
        FunctionParsingInsights parsing_insights;
        parsing_insights.uses_this_from_environment = true;
        parsing_insights.uses_this = true;
        initializer = ECMAScriptFunctionObject::create(realm, "field"_string, ByteString::empty(), *function_code, FunctionParameters::empty(), 0, {}, vm.lexical_environment(), vm.running_execution_context().private_environment, FunctionKind::Normal, true, parsing_insights, false, property_key_or_private_name);
        initializer->make_method(target);
    }

    return ClassValue {
        ClassFieldDefinition {
            move(property_key_or_private_name),
            move(initializer),
        }
    };
}

static Optional<FlyString> nullopt_or_private_identifier_description(Expression const& expression)
{
    if (is<PrivateIdentifier>(expression))
        return static_cast<PrivateIdentifier const&>(expression).string();
    return {};
}

Optional<FlyString> ClassField::private_bound_identifier() const
{
    return nullopt_or_private_identifier_description(*m_key);
}

Optional<FlyString> ClassMethod::private_bound_identifier() const
{
    return nullopt_or_private_identifier_description(*m_key);
}

// 15.7.11 Runtime Semantics: ClassStaticBlockDefinitionEvaluation, https://tc39.es/ecma262/#sec-runtime-semantics-classstaticblockdefinitionevaluation
ThrowCompletionOr<ClassElement::ClassValue> StaticInitializer::class_element_evaluation(VM& vm, Object& home_object, Value) const
{
    auto& realm = *vm.current_realm();

    // 1. Let lex be the running execution context's LexicalEnvironment.
    auto lexical_environment = vm.running_execution_context().lexical_environment;

    // 2. Let privateEnv be the running execution context's PrivateEnvironment.
    auto private_environment = vm.running_execution_context().private_environment;

    // 3. Let sourceText be the empty sequence of Unicode code points.
    // 4. Let formalParameters be an instance of the production FormalParameters : [empty] .
    // 5. Let bodyFunction be OrdinaryFunctionCreate(%Function.prototype%, sourceText, formalParameters, ClassStaticBlockBody, non-lexical-this, lex, privateEnv).
    // Note: The function bodyFunction is never directly accessible to ECMAScript code.
    FunctionParsingInsights parsing_insights;
    parsing_insights.uses_this_from_environment = true;
    parsing_insights.uses_this = true;
    auto body_function = ECMAScriptFunctionObject::create(realm, ""_string, ByteString::empty(), *m_function_body, FunctionParameters::empty(), 0, m_function_body->local_variables_names(), lexical_environment, private_environment, FunctionKind::Normal, true, parsing_insights, false);

    // 6. Perform MakeMethod(bodyFunction, homeObject).
    body_function->make_method(home_object);

    // 7. Return the ClassStaticBlockDefinition Record { [[BodyFunction]]: bodyFunction }.
    return ClassValue { normal_completion(body_function) };
}

ThrowCompletionOr<ECMAScriptFunctionObject*> ClassExpression::create_class_constructor(VM& vm, Environment* class_environment, Environment* environment, Value super_class, ReadonlySpan<Value> element_keys, Optional<FlyString> const& binding_name, FlyString const& class_name) const
{
    auto& realm = *vm.current_realm();

    // We might not set the lexical environment but we always want to restore it eventually.
    ArmedScopeGuard restore_environment = [&] {
        vm.running_execution_context().lexical_environment = environment;
    };

    vm.running_execution_context().lexical_environment = class_environment;

    auto proto_parent = GC::Ptr { realm.intrinsics().object_prototype() };
    auto constructor_parent = realm.intrinsics().function_prototype();

    if (!m_super_class.is_null()) {
        if (super_class.is_null()) {
            proto_parent = nullptr;
        } else if (!super_class.is_constructor()) {
            return vm.throw_completion<TypeError>(ErrorType::ClassExtendsValueNotAConstructorOrNull, super_class.to_string_without_side_effects());
        } else {
            auto super_class_prototype = TRY(super_class.get(vm, vm.names.prototype));
            if (!super_class_prototype.is_null() && !super_class_prototype.is_object())
                return vm.throw_completion<TypeError>(ErrorType::ClassExtendsValueInvalidPrototype, super_class_prototype.to_string_without_side_effects());

            if (super_class_prototype.is_null())
                proto_parent = nullptr;
            else
                proto_parent = super_class_prototype.as_object();

            constructor_parent = super_class.as_object();
        }
    }

    auto prototype = Object::create_prototype(realm, proto_parent);
    VERIFY(prototype);

    // FIXME: Step 14.a is done in the parser. By using a synthetic super(...args) which does not call @@iterator of %Array.prototype%
    auto const& constructor = *m_constructor;
    auto parsing_insights = constructor.parsing_insights();
    parsing_insights.uses_this_from_environment = true;
    parsing_insights.uses_this = true;
    auto class_constructor = ECMAScriptFunctionObject::create(
        realm,
        constructor.name(),
        constructor.source_text(),
        constructor.body(),
        constructor.parameters(),
        constructor.function_length(),
        constructor.local_variables_names(),
        vm.lexical_environment(),
        vm.running_execution_context().private_environment,
        constructor.kind(),
        constructor.is_strict_mode(),
        parsing_insights,
        constructor.is_arrow_function());

    class_constructor->set_name(class_name);
    class_constructor->set_home_object(prototype);
    class_constructor->set_is_class_constructor();
    class_constructor->define_direct_property(vm.names.prototype, prototype, Attribute::Writable);
    TRY(class_constructor->internal_set_prototype_of(constructor_parent));

    if (!m_super_class.is_null())
        class_constructor->set_constructor_kind(ECMAScriptFunctionObject::ConstructorKind::Derived);

    prototype->define_direct_property(vm.names.constructor, class_constructor, Attribute::Writable | Attribute::Configurable);

    using StaticElement = Variant<ClassFieldDefinition, GC::Ref<ECMAScriptFunctionObject>>;

    GC::ConservativeVector<PrivateElement> static_private_methods(vm.heap());
    GC::ConservativeVector<PrivateElement> instance_private_methods(vm.heap());
    GC::ConservativeVector<ClassFieldDefinition> instance_fields(vm.heap());
    GC::ConservativeVector<StaticElement> static_elements(vm.heap());

    for (size_t element_index = 0; element_index < m_elements.size(); element_index++) {
        auto const& element = m_elements[element_index];

        // Note: All ClassElementEvaluation start with evaluating the name (or we fake it).
        auto element_value = TRY(element->class_element_evaluation(vm, element->is_static() ? *class_constructor : *prototype, element_keys[element_index]));

        if (element_value.has<PrivateElement>()) {
            auto& container = element->is_static() ? static_private_methods : instance_private_methods;

            auto& private_element = element_value.get<PrivateElement>();

            auto added_to_existing = false;
            // FIXME: We can skip this loop in most cases.
            for (auto& existing : container) {
                if (existing.key == private_element.key) {
                    VERIFY(existing.kind == PrivateElement::Kind::Accessor);
                    VERIFY(private_element.kind == PrivateElement::Kind::Accessor);
                    auto& accessor = private_element.value.as_accessor();
                    if (!accessor.getter())
                        existing.value.as_accessor().set_setter(accessor.setter());
                    else
                        existing.value.as_accessor().set_getter(accessor.getter());
                    added_to_existing = true;
                }
            }

            if (!added_to_existing)
                container.append(move(element_value.get<PrivateElement>()));
        } else if (auto* class_field_definition_ptr = element_value.get_pointer<ClassFieldDefinition>()) {
            if (element->is_static())
                static_elements.append(move(*class_field_definition_ptr));
            else
                instance_fields.append(move(*class_field_definition_ptr));
        } else if (element->class_element_kind() == ClassElement::ElementKind::StaticInitializer) {
            // We use Completion to hold the ClassStaticBlockDefinition Record.
            VERIFY(element_value.has<Completion>() && element_value.get<Completion>().value().has_value());
            auto& element_object = element_value.get<Completion>().value()->as_object();
            VERIFY(is<ECMAScriptFunctionObject>(element_object));
            static_elements.append(GC::Ref { static_cast<ECMAScriptFunctionObject&>(element_object) });
        }
    }

    vm.running_execution_context().lexical_environment = environment;
    restore_environment.disarm();

    if (binding_name.has_value())
        MUST(class_environment->initialize_binding(vm, binding_name.value(), class_constructor, Environment::InitializeBindingHint::Normal));

    for (auto& field : instance_fields)
        class_constructor->add_field(field);

    for (auto& private_method : instance_private_methods)
        class_constructor->add_private_method(private_method);

    for (auto& method : static_private_methods)
        TRY(class_constructor->private_method_or_accessor_add(move(method)));

    for (auto& element : static_elements) {
        TRY(element.visit(
            [&](ClassFieldDefinition& field) -> ThrowCompletionOr<void> {
                return TRY(class_constructor->define_field(field));
            },
            [&](GC::Root<ECMAScriptFunctionObject> static_block_function) -> ThrowCompletionOr<void> {
                VERIFY(!static_block_function.is_null());
                // We discard any value returned here.
                TRY(call(vm, *static_block_function.cell(), class_constructor));
                return {};
            }));
    }

    class_constructor->set_source_text(source_text());

    return { class_constructor };
}

void ASTNode::dump(int indent) const
{
    print_indent(indent);
    outln("{}", class_name());
}

void ScopeNode::dump(int indent) const
{
    ASTNode::dump(indent);
    if (!m_children.is_empty()) {
        print_indent(indent + 1);
        outln("(Children)");
        for (auto& child : children())
            child->dump(indent + 2);
    }
}

void BinaryExpression::dump(int indent) const
{
    char const* op_string = nullptr;
    switch (m_op) {
    case BinaryOp::Addition:
        op_string = "+";
        break;
    case BinaryOp::Subtraction:
        op_string = "-";
        break;
    case BinaryOp::Multiplication:
        op_string = "*";
        break;
    case BinaryOp::Division:
        op_string = "/";
        break;
    case BinaryOp::Modulo:
        op_string = "%";
        break;
    case BinaryOp::Exponentiation:
        op_string = "**";
        break;
    case BinaryOp::StrictlyEquals:
        op_string = "===";
        break;
    case BinaryOp::StrictlyInequals:
        op_string = "!==";
        break;
    case BinaryOp::LooselyEquals:
        op_string = "==";
        break;
    case BinaryOp::LooselyInequals:
        op_string = "!=";
        break;
    case BinaryOp::GreaterThan:
        op_string = ">";
        break;
    case BinaryOp::GreaterThanEquals:
        op_string = ">=";
        break;
    case BinaryOp::LessThan:
        op_string = "<";
        break;
    case BinaryOp::LessThanEquals:
        op_string = "<=";
        break;
    case BinaryOp::BitwiseAnd:
        op_string = "&";
        break;
    case BinaryOp::BitwiseOr:
        op_string = "|";
        break;
    case BinaryOp::BitwiseXor:
        op_string = "^";
        break;
    case BinaryOp::LeftShift:
        op_string = "<<";
        break;
    case BinaryOp::RightShift:
        op_string = ">>";
        break;
    case BinaryOp::UnsignedRightShift:
        op_string = ">>>";
        break;
    case BinaryOp::In:
        op_string = "in";
        break;
    case BinaryOp::InstanceOf:
        op_string = "instanceof";
        break;
    }

    print_indent(indent);
    outln("{}", class_name());
    m_lhs->dump(indent + 1);
    print_indent(indent + 1);
    outln("{}", op_string);
    m_rhs->dump(indent + 1);
}

void LogicalExpression::dump(int indent) const
{
    char const* op_string = nullptr;
    switch (m_op) {
    case LogicalOp::And:
        op_string = "&&";
        break;
    case LogicalOp::Or:
        op_string = "||";
        break;
    case LogicalOp::NullishCoalescing:
        op_string = "??";
        break;
    }

    print_indent(indent);
    outln("{}", class_name());
    m_lhs->dump(indent + 1);
    print_indent(indent + 1);
    outln("{}", op_string);
    m_rhs->dump(indent + 1);
}

void UnaryExpression::dump(int indent) const
{
    char const* op_string = nullptr;
    switch (m_op) {
    case UnaryOp::BitwiseNot:
        op_string = "~";
        break;
    case UnaryOp::Not:
        op_string = "!";
        break;
    case UnaryOp::Plus:
        op_string = "+";
        break;
    case UnaryOp::Minus:
        op_string = "-";
        break;
    case UnaryOp::Typeof:
        op_string = "typeof ";
        break;
    case UnaryOp::Void:
        op_string = "void ";
        break;
    case UnaryOp::Delete:
        op_string = "delete ";
        break;
    }

    print_indent(indent);
    outln("{}", class_name());
    print_indent(indent + 1);
    outln("{}", op_string);
    m_lhs->dump(indent + 1);
}

void CallExpression::dump(int indent) const
{
    print_indent(indent);
    if (is<NewExpression>(*this))
        outln("CallExpression [new]");
    else
        outln("CallExpression");
    m_callee->dump(indent + 1);
    for (auto& argument : arguments())
        argument.value->dump(indent + 1);
}

void SuperCall::dump(int indent) const
{
    print_indent(indent);
    outln("SuperCall");
    for (auto& argument : m_arguments)
        argument.value->dump(indent + 1);
}

void ClassDeclaration::dump(int indent) const
{
    ASTNode::dump(indent);
    m_class_expression->dump(indent + 1);
}

ThrowCompletionOr<void> ClassDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    if (!m_class_expression->m_name)
        return {};

    return callback(*m_class_expression->m_name);
}

void ClassExpression::dump(int indent) const
{
    print_indent(indent);
    outln("ClassExpression: \"{}\"", name());

    print_indent(indent);
    outln("(Constructor)");
    m_constructor->dump(indent + 1);

    if (!m_super_class.is_null()) {
        print_indent(indent);
        outln("(Super Class)");
        m_super_class->dump(indent + 1);
    }

    print_indent(indent);
    outln("(Elements)");
    for (auto& method : m_elements)
        method->dump(indent + 1);
}

void ClassMethod::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("(Key)");
    m_key->dump(indent + 1);

    char const* kind_string = nullptr;
    switch (m_kind) {
    case Kind::Method:
        kind_string = "Method";
        break;
    case Kind::Getter:
        kind_string = "Getter";
        break;
    case Kind::Setter:
        kind_string = "Setter";
        break;
    }
    print_indent(indent);
    outln("Kind: {}", kind_string);

    print_indent(indent);
    outln("Static: {}", is_static());

    print_indent(indent);
    outln("(Function)");
    m_function->dump(indent + 1);
}

void ClassField::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent);
    outln("(Key)");
    m_key->dump(indent + 1);

    print_indent(indent);
    outln("Static: {}", is_static());

    if (m_initializer) {
        print_indent(indent);
        outln("(Initializer)");
        m_initializer->dump(indent + 1);
    }
}

void StaticInitializer::dump(int indent) const
{
    ASTNode::dump(indent);
    m_function_body->dump(indent + 1);
}

void StringLiteral::dump(int indent) const
{
    print_indent(indent);
    outln("StringLiteral \"{}\"", m_value);
}

void SuperExpression::dump(int indent) const
{
    print_indent(indent);
    outln("super");
}

void NumericLiteral::dump(int indent) const
{
    print_indent(indent);
    outln("NumericLiteral {}", m_value);
}

void BigIntLiteral::dump(int indent) const
{
    print_indent(indent);
    outln("BigIntLiteral {}", m_value);
}

void BooleanLiteral::dump(int indent) const
{
    print_indent(indent);
    outln("BooleanLiteral {}", m_value);
}

void NullLiteral::dump(int indent) const
{
    print_indent(indent);
    outln("null");
}

bool BindingPattern::contains_expression() const
{
    for (auto& entry : entries) {
        if (entry.name.has<NonnullRefPtr<Expression const>>())
            return true;
        if (entry.initializer)
            return true;
        if (auto binding_ptr = entry.alias.get_pointer<NonnullRefPtr<BindingPattern const>>(); binding_ptr && (*binding_ptr)->contains_expression())
            return true;
    }
    return false;
}

ThrowCompletionOr<void> BindingPattern::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& entry : entries) {
        auto const& alias = entry.alias;
        if (alias.has<NonnullRefPtr<Identifier const>>()) {
            TRY(callback(alias.get<NonnullRefPtr<Identifier const>>()));
        } else if (alias.has<NonnullRefPtr<BindingPattern const>>()) {
            TRY(alias.get<NonnullRefPtr<BindingPattern const>>()->for_each_bound_identifier(forward<decltype(callback)>(callback)));
        } else {
            auto const& name = entry.name;
            if (name.has<NonnullRefPtr<Identifier const>>())
                TRY(callback(name.get<NonnullRefPtr<Identifier const>>()));
        }
    }
    return {};
}

void BindingPattern::dump(int indent) const
{
    print_indent(indent);
    outln("BindingPattern {}", kind == Kind::Array ? "Array" : "Object");

    for (auto& entry : entries) {
        print_indent(indent + 1);
        outln("(Property)");

        if (kind == Kind::Object) {
            print_indent(indent + 2);
            outln("(Identifier)");
            if (entry.name.has<NonnullRefPtr<Identifier const>>()) {
                entry.name.get<NonnullRefPtr<Identifier const>>()->dump(indent + 3);
            } else if (entry.name.has<NonnullRefPtr<Expression const>>()) {
                entry.name.get<NonnullRefPtr<Expression const>>()->dump(indent + 3);
            } else {
                VERIFY(entry.name.has<Empty>());
                print_indent(indent + 3);
                outln("<empty>");
            }
        } else if (entry.is_elision()) {
            print_indent(indent + 2);
            outln("(Elision)");
            continue;
        }

        print_indent(indent + 2);
        outln("(Pattern{})", entry.is_rest ? " rest=true" : "");
        if (entry.alias.has<NonnullRefPtr<Identifier const>>()) {
            entry.alias.get<NonnullRefPtr<Identifier const>>()->dump(indent + 3);
        } else if (entry.alias.has<NonnullRefPtr<BindingPattern const>>()) {
            entry.alias.get<NonnullRefPtr<BindingPattern const>>()->dump(indent + 3);
        } else if (entry.alias.has<NonnullRefPtr<MemberExpression const>>()) {
            entry.alias.get<NonnullRefPtr<MemberExpression const>>()->dump(indent + 3);
        } else {
            print_indent(indent + 3);
            outln("<empty>");
        }

        if (entry.initializer) {
            print_indent(indent + 2);
            outln("(Initializer)");
            entry.initializer->dump(indent + 3);
        }
    }
}

void FunctionNode::dump(int indent, ByteString const& class_name) const
{
    print_indent(indent);
    auto is_async = m_kind == FunctionKind::Async || m_kind == FunctionKind::AsyncGenerator;
    auto is_generator = m_kind == FunctionKind::Generator || m_kind == FunctionKind::AsyncGenerator;
    outln("{}{}{} '{}'", class_name, is_async ? " async" : "", is_generator ? "*" : "", name());
    if (m_parsing_insights.contains_direct_call_to_eval) {
        print_indent(indent + 1);
        outln("\033[31;1m(direct eval)\033[0m");
    }
    if (!m_parameters->is_empty()) {
        print_indent(indent + 1);
        outln("(Parameters)");

        for (auto& parameter : m_parameters->parameters()) {
            parameter.binding.visit(
                [&](Identifier const& identifier) {
                    if (parameter.is_rest) {
                        print_indent(indent + 2);
                        out("...");
                        identifier.dump(0);
                    } else {
                        identifier.dump(indent + 2);
                    }
                },
                [&](BindingPattern const& pattern) {
                    pattern.dump(indent + 2);
                });
            if (parameter.default_value)
                parameter.default_value->dump(indent + 3);
        }
    }
    print_indent(indent + 1);
    outln("(Body)");
    body().dump(indent + 2);
}

void FunctionDeclaration::dump(int indent) const
{
    FunctionNode::dump(indent, class_name());
}

ThrowCompletionOr<void> FunctionDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    if (!m_name)
        return {};
    return callback(*m_name);
}

void FunctionExpression::dump(int indent) const
{
    FunctionNode::dump(indent, class_name());
}

void YieldExpression::dump(int indent) const
{
    ASTNode::dump(indent);
    if (argument())
        argument()->dump(indent + 1);
}

void AwaitExpression::dump(int indent) const
{
    ASTNode::dump(indent);
    m_argument->dump(indent + 1);
}

void ReturnStatement::dump(int indent) const
{
    ASTNode::dump(indent);
    if (argument())
        argument()->dump(indent + 1);
}

void IfStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("If");
    predicate().dump(indent + 1);
    consequent().dump(indent + 1);
    if (alternate()) {
        print_indent(indent);
        outln("Else");
        alternate()->dump(indent + 1);
    }
}

void WhileStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("While");
    test().dump(indent + 1);
    body().dump(indent + 1);
}

void WithStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent + 1);
    outln("Object");
    object().dump(indent + 2);
    print_indent(indent + 1);
    outln("Body");
    body().dump(indent + 2);
}

void DoWhileStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("DoWhile");
    test().dump(indent + 1);
    body().dump(indent + 1);
}

void ForStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("For");
    if (init())
        init()->dump(indent + 1);
    if (test())
        test()->dump(indent + 1);
    if (update())
        update()->dump(indent + 1);
    body().dump(indent + 1);
}

void ForInStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("ForIn");
    lhs().visit([&](auto& lhs) { lhs->dump(indent + 1); });
    rhs().dump(indent + 1);
    body().dump(indent + 1);
}

void ForOfStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("ForOf");
    lhs().visit([&](auto& lhs) { lhs->dump(indent + 1); });
    rhs().dump(indent + 1);
    body().dump(indent + 1);
}

void ForAwaitOfStatement::dump(int indent) const
{
    ASTNode::dump(indent);

    print_indent(indent);
    outln("ForAwaitOf");
    m_lhs.visit([&](auto& lhs) { lhs->dump(indent + 1); });
    m_rhs->dump(indent + 1);
    m_body->dump(indent + 1);
}

void Identifier::dump(int indent) const
{
    print_indent(indent);
    if (is_local()) {
        outln("Identifier \"{}\" is_local=(true) index=({})", m_string, m_local_variable_index);
    } else if (is_global()) {
        outln("Identifier \"{}\" is_global=(true)", m_string);
    } else {
        outln("Identifier \"{}\"", m_string);
    }
}

void PrivateIdentifier::dump(int indent) const
{
    print_indent(indent);
    outln("PrivateIdentifier \"{}\"", m_string);
}

void SpreadExpression::dump(int indent) const
{
    ASTNode::dump(indent);
    m_target->dump(indent + 1);
}

void ThisExpression::dump(int indent) const
{
    ASTNode::dump(indent);
}

void AssignmentExpression::dump(int indent) const
{
    char const* op_string = nullptr;
    switch (m_op) {
    case AssignmentOp::Assignment:
        op_string = "=";
        break;
    case AssignmentOp::AdditionAssignment:
        op_string = "+=";
        break;
    case AssignmentOp::SubtractionAssignment:
        op_string = "-=";
        break;
    case AssignmentOp::MultiplicationAssignment:
        op_string = "*=";
        break;
    case AssignmentOp::DivisionAssignment:
        op_string = "/=";
        break;
    case AssignmentOp::ModuloAssignment:
        op_string = "%=";
        break;
    case AssignmentOp::ExponentiationAssignment:
        op_string = "**=";
        break;
    case AssignmentOp::BitwiseAndAssignment:
        op_string = "&=";
        break;
    case AssignmentOp::BitwiseOrAssignment:
        op_string = "|=";
        break;
    case AssignmentOp::BitwiseXorAssignment:
        op_string = "^=";
        break;
    case AssignmentOp::LeftShiftAssignment:
        op_string = "<<=";
        break;
    case AssignmentOp::RightShiftAssignment:
        op_string = ">>=";
        break;
    case AssignmentOp::UnsignedRightShiftAssignment:
        op_string = ">>>=";
        break;
    case AssignmentOp::AndAssignment:
        op_string = "&&=";
        break;
    case AssignmentOp::OrAssignment:
        op_string = "||=";
        break;
    case AssignmentOp::NullishAssignment:
        op_string = "\?\?=";
        break;
    }

    ASTNode::dump(indent);
    print_indent(indent + 1);
    outln("{}", op_string);
    m_lhs.visit([&](auto& lhs) { lhs->dump(indent + 1); });
    m_rhs->dump(indent + 1);
}

void UpdateExpression::dump(int indent) const
{
    char const* op_string = nullptr;
    switch (m_op) {
    case UpdateOp::Increment:
        op_string = "++";
        break;
    case UpdateOp::Decrement:
        op_string = "--";
        break;
    }

    ASTNode::dump(indent);
    if (m_prefixed) {
        print_indent(indent + 1);
        outln("{}", op_string);
    }
    m_argument->dump(indent + 1);
    if (!m_prefixed) {
        print_indent(indent + 1);
        outln("{}", op_string);
    }
}

ThrowCompletionOr<void> VariableDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& entry : declarations()) {
        TRY(entry->target().visit(
            [&](NonnullRefPtr<Identifier const> const& id) {
                return callback(id);
            },
            [&](NonnullRefPtr<BindingPattern const> const& binding) {
                return binding->for_each_bound_identifier([&](auto const& id) {
                    return callback(id);
                });
            }));
    }

    return {};
}

void VariableDeclaration::dump(int indent) const
{
    char const* declaration_kind_string = nullptr;
    switch (m_declaration_kind) {
    case DeclarationKind::Let:
        declaration_kind_string = "Let";
        break;
    case DeclarationKind::Var:
        declaration_kind_string = "Var";
        break;
    case DeclarationKind::Const:
        declaration_kind_string = "Const";
        break;
    }

    ASTNode::dump(indent);
    print_indent(indent + 1);
    outln("{}", declaration_kind_string);

    for (auto& declarator : m_declarations)
        declarator->dump(indent + 1);
}

ThrowCompletionOr<void> UsingDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& entry : m_declarations) {
        VERIFY(entry->target().has<NonnullRefPtr<Identifier const>>());
        TRY(callback(entry->target().get<NonnullRefPtr<Identifier const>>()));
    }

    return {};
}

void UsingDeclaration::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent + 1);
    for (auto& declarator : m_declarations)
        declarator->dump(indent + 1);
}

void VariableDeclarator::dump(int indent) const
{
    ASTNode::dump(indent);
    m_target.visit([indent](auto const& value) { value->dump(indent + 1); });
    if (m_init)
        m_init->dump(indent + 1);
}

void ObjectProperty::dump(int indent) const
{
    ASTNode::dump(indent);

    if (m_property_type == Type::Spread) {
        print_indent(indent + 1);
        outln("...Spreading");
        m_key->dump(indent + 1);
    } else {
        m_key->dump(indent + 1);
        m_value->dump(indent + 1);
    }
}

void ObjectExpression::dump(int indent) const
{
    ASTNode::dump(indent);
    for (auto& property : m_properties) {
        property->dump(indent + 1);
    }
}

void ExpressionStatement::dump(int indent) const
{
    ASTNode::dump(indent);
    m_expression->dump(indent + 1);
}

void MemberExpression::dump(int indent) const
{
    print_indent(indent);
    outln("{}(computed={})", class_name(), is_computed());
    m_object->dump(indent + 1);
    m_property->dump(indent + 1);
}

String MemberExpression::to_string_approximation() const
{
    String object_string = "<object>"_string;
    if (is<Identifier>(*m_object))
        object_string = static_cast<Identifier const&>(*m_object).string().to_string();
    if (is_computed())
        return MUST(String::formatted("{}[<computed>]", object_string));
    if (is<PrivateIdentifier>(*m_property))
        return MUST(String::formatted("{}.{}", object_string, as<PrivateIdentifier>(*m_property).string()));
    return MUST(String::formatted("{}.{}", object_string, as<Identifier>(*m_property).string()));
}

bool MemberExpression::ends_in_private_name() const
{
    if (is_computed())
        return false;
    if (is<PrivateIdentifier>(*m_property))
        return true;
    if (is<MemberExpression>(*m_property))
        return static_cast<MemberExpression const&>(*m_property).ends_in_private_name();
    return false;
}

void OptionalChain::dump(int indent) const
{
    print_indent(indent);
    outln("{}", class_name());
    m_base->dump(indent + 1);
    for (auto& reference : m_references) {
        reference.visit(
            [&](Call const& call) {
                print_indent(indent + 1);
                outln("Call({})", call.mode == Mode::Optional ? "Optional" : "Not Optional");
                for (auto& argument : call.arguments)
                    argument.value->dump(indent + 2);
            },
            [&](ComputedReference const& ref) {
                print_indent(indent + 1);
                outln("ComputedReference({})", ref.mode == Mode::Optional ? "Optional" : "Not Optional");
                ref.expression->dump(indent + 2);
            },
            [&](MemberReference const& ref) {
                print_indent(indent + 1);
                outln("MemberReference({})", ref.mode == Mode::Optional ? "Optional" : "Not Optional");
                ref.identifier->dump(indent + 2);
            },
            [&](PrivateMemberReference const& ref) {
                print_indent(indent + 1);
                outln("PrivateMemberReference({})", ref.mode == Mode::Optional ? "Optional" : "Not Optional");
                ref.private_identifier->dump(indent + 2);
            });
    }
}

void MetaProperty::dump(int indent) const
{
    ByteString name;
    if (m_type == MetaProperty::Type::NewTarget)
        name = "new.target";
    else if (m_type == MetaProperty::Type::ImportMeta)
        name = "import.meta";
    else
        VERIFY_NOT_REACHED();
    print_indent(indent);
    outln("{} {}", class_name(), name);
}

void ImportCall::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent);
    outln("(Specifier)");
    m_specifier->dump(indent + 1);
    if (m_options) {
        outln("(Options)");
        m_options->dump(indent + 1);
    }
}

void RegExpLiteral::dump(int indent) const
{
    print_indent(indent);
    outln("{} (/{}/{})", class_name(), pattern(), flags());
}

void ArrayExpression::dump(int indent) const
{
    ASTNode::dump(indent);
    for (auto& element : m_elements) {
        if (element) {
            element->dump(indent + 1);
        } else {
            print_indent(indent + 1);
            outln("<empty>");
        }
    }
}

void TemplateLiteral::dump(int indent) const
{
    ASTNode::dump(indent);
    for (auto& expression : m_expressions)
        expression->dump(indent + 1);
}

void TaggedTemplateLiteral::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent + 1);
    outln("(Tag)");
    m_tag->dump(indent + 2);
    print_indent(indent + 1);
    outln("(Template Literal)");
    m_template_literal->dump(indent + 2);
}

void TryStatement::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent);
    outln("(Block)");
    block().dump(indent + 1);

    if (handler()) {
        print_indent(indent);
        outln("(Handler)");
        handler()->dump(indent + 1);
    }

    if (finalizer()) {
        print_indent(indent);
        outln("(Finalizer)");
        finalizer()->dump(indent + 1);
    }
}

void CatchClause::dump(int indent) const
{
    print_indent(indent);
    m_parameter.visit(
        [&](FlyString const& parameter) {
            if (parameter.is_empty())
                outln("CatchClause");
            else
                outln("CatchClause ({})", parameter);
        },
        [&](NonnullRefPtr<BindingPattern const> const& pattern) {
            outln("CatchClause");
            print_indent(indent);
            outln("(Parameter)");
            pattern->dump(indent + 2);
        });

    body().dump(indent + 1);
}

void ThrowStatement::dump(int indent) const
{
    ASTNode::dump(indent);
    argument().dump(indent + 1);
}

void SwitchStatement::dump(int indent) const
{
    ASTNode::dump(indent);
    m_discriminant->dump(indent + 1);
    for (auto& switch_case : m_cases) {
        switch_case->dump(indent + 1);
    }
}

void SwitchCase::dump(int indent) const
{
    print_indent(indent + 1);
    if (m_test) {
        outln("(Test)");
        m_test->dump(indent + 2);
    } else {
        outln("(Default)");
    }
    print_indent(indent + 1);
    outln("(Consequent)");
    ScopeNode::dump(indent + 2);
}

void ConditionalExpression::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent + 1);
    outln("(Test)");
    m_test->dump(indent + 2);
    print_indent(indent + 1);
    outln("(Consequent)");
    m_consequent->dump(indent + 2);
    print_indent(indent + 1);
    outln("(Alternate)");
    m_alternate->dump(indent + 2);
}

void SequenceExpression::dump(int indent) const
{
    ASTNode::dump(indent);
    for (auto& expression : m_expressions)
        expression->dump(indent + 1);
}

bool ScopeNode::has_non_local_lexical_declarations() const
{
    bool result = false;
    MUST(for_each_lexically_declared_identifier([&](Identifier const& identifier) {
        if (!identifier.is_local())
            result = true;
    }));
    return result;
}

ThrowCompletionOr<void> ScopeNode::for_each_lexically_scoped_declaration(ThrowCompletionOrVoidCallback<Declaration const&>&& callback) const
{
    for (auto& declaration : m_lexical_declarations)
        TRY(callback(declaration));

    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_lexically_declared_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& declaration : m_lexical_declarations) {
        TRY(declaration->for_each_bound_identifier([&](auto const& identifier) {
            return callback(identifier);
        }));
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_var_declared_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto& declaration : m_var_declarations) {
        TRY(declaration->for_each_bound_identifier([&](auto const& id) {
            return callback(id);
        }));
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_var_function_declaration_in_reverse_order(ThrowCompletionOrVoidCallback<FunctionDeclaration const&>&& callback) const
{
    for (ssize_t i = m_var_declarations.size() - 1; i >= 0; i--) {
        auto& declaration = m_var_declarations[i];
        if (is<FunctionDeclaration>(declaration))
            TRY(callback(static_cast<FunctionDeclaration const&>(*declaration)));
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_var_scoped_variable_declaration(ThrowCompletionOrVoidCallback<VariableDeclaration const&>&& callback) const
{
    for (auto& declaration : m_var_declarations) {
        if (!is<FunctionDeclaration>(declaration)) {
            VERIFY(is<VariableDeclaration>(declaration));
            TRY(callback(static_cast<VariableDeclaration const&>(*declaration)));
        }
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_function_hoistable_with_annexB_extension(ThrowCompletionOrVoidCallback<FunctionDeclaration&>&& callback) const
{
    for (auto& function : m_functions_hoistable_with_annexB_extension) {
        // We need const_cast here since it might have to set a property on function declaration.
        TRY(callback(const_cast<FunctionDeclaration&>(*function)));
    }
    return {};
}

void ScopeNode::add_lexical_declaration(NonnullRefPtr<Declaration const> declaration)
{
    m_lexical_declarations.append(move(declaration));
}

void ScopeNode::add_var_scoped_declaration(NonnullRefPtr<Declaration const> declaration)
{
    m_var_declarations.append(move(declaration));
}

void ScopeNode::add_hoisted_function(NonnullRefPtr<FunctionDeclaration const> declaration)
{
    m_functions_hoistable_with_annexB_extension.append(move(declaration));
}

FlyString ExportStatement::local_name_for_default = "*default*"_fly_string;

static void dump_assert_clauses(ModuleRequest const& request)
{
    if (!request.attributes.is_empty()) {
        out("[ ");
        for (auto& assertion : request.attributes)
            out("{}: {}, ", assertion.key, assertion.value);
        out(" ]");
    }
}

void ExportStatement::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent + 1);
    outln("(ExportEntries)");

    auto string_or_null = [](Optional<FlyString> const& string) -> ByteString {
        if (!string.has_value()) {
            return "null";
        }
        return ByteString::formatted("\"{}\"", string);
    };

    for (auto& entry : m_entries) {
        print_indent(indent + 2);
        out("ExportName: {}, ImportName: {}, LocalName: {}, ModuleRequest: ",
            string_or_null(entry.export_name),
            entry.is_module_request() ? string_or_null(entry.local_or_import_name) : "null",
            entry.is_module_request() ? "null" : string_or_null(entry.local_or_import_name));
        if (entry.is_module_request()) {
            out("{}", entry.m_module_request->module_specifier);
            dump_assert_clauses(*entry.m_module_request);
            outln();
        } else {
            outln("null");
        }
    }

    if (m_statement) {
        print_indent(indent + 1);
        outln("(Statement)");
        m_statement->dump(indent + 2);
    }
}

void ImportStatement::dump(int indent) const
{
    ASTNode::dump(indent);
    print_indent(indent + 1);
    if (m_entries.is_empty()) {
        // direct from "module" import
        outln("Entire module '{}'", m_module_request.module_specifier);
        dump_assert_clauses(m_module_request);
    } else {
        outln("(ExportEntries) from {}", m_module_request.module_specifier);
        dump_assert_clauses(m_module_request);

        for (auto& entry : m_entries) {
            print_indent(indent + 2);
            outln("ImportName: {}, LocalName: {}", entry.import_name, entry.local_name);
        }
    }
}

bool ExportStatement::has_export(FlyString const& export_name) const
{
    return any_of(m_entries.begin(), m_entries.end(), [&](auto& entry) {
        // Make sure that empty exported names does not overlap with anything
        if (entry.kind != ExportEntry::Kind::NamedExport)
            return false;
        return entry.export_name == export_name;
    });
}

bool ImportStatement::has_bound_name(FlyString const& name) const
{
    return any_of(m_entries.begin(), m_entries.end(), [&](auto& entry) {
        return entry.local_name == name;
    });
}

// 14.2.3 BlockDeclarationInstantiation ( code, env ), https://tc39.es/ecma262/#sec-blockdeclarationinstantiation
void ScopeNode::block_declaration_instantiation(VM& vm, Environment* environment) const
{
    // See also B.3.2.6 Changes to BlockDeclarationInstantiation, https://tc39.es/ecma262/#sec-web-compat-blockdeclarationinstantiation
    auto& realm = *vm.current_realm();

    VERIFY(environment);

    // 1. Let declarations be the LexicallyScopedDeclarations of code.

    // 2. Let privateEnv be the running execution context's PrivateEnvironment.
    auto private_environment = vm.running_execution_context().private_environment;

    // Note: All the calls here are ! and thus we do not need to TRY this callback.
    //       We use MUST to ensure it does not throw and to avoid discarding the returned ThrowCompletionOr<void>.
    // 3. For each element d of declarations, do
    MUST(for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        auto is_constant_declaration = declaration.is_constant_declaration();
        // NOTE: Due to the use of MUST with `create_immutable_binding` and `create_mutable_binding` below,
        //       an exception should not result from `for_each_bound_name`.
        // a. For each element dn of the BoundNames of d, do
        MUST(declaration.for_each_bound_identifier([&](auto const& identifier) {
            if (identifier.is_local()) {
                // NOTE: No need to create bindings for local variables as their values are not stored in an environment.
                return;
            }

            auto const& name = identifier.string();
            // i. If IsConstantDeclaration of d is true, then
            if (is_constant_declaration) {
                // 1. Perform ! env.CreateImmutableBinding(dn, true).
                MUST(environment->create_immutable_binding(vm, name, true));
            }
            // ii. Else,
            else {
                // 1. Perform ! env.CreateMutableBinding(dn, false). NOTE: This step is replaced in section B.3.2.6.
                if (!MUST(environment->has_binding(name)))
                    MUST(environment->create_mutable_binding(vm, name, false));
            }
        }));

        // b. If d is either a FunctionDeclaration, a GeneratorDeclaration, an AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration, then
        if (is<FunctionDeclaration>(declaration)) {
            // i. Let fn be the sole element of the BoundNames of d.
            auto& function_declaration = static_cast<FunctionDeclaration const&>(declaration);

            // ii. Let fo be InstantiateFunctionObject of d with arguments env and privateEnv.
            auto function = ECMAScriptFunctionObject::create(realm, function_declaration.name(), function_declaration.source_text(), function_declaration.body(), function_declaration.parameters(), function_declaration.function_length(), function_declaration.local_variables_names(), environment, private_environment, function_declaration.kind(), function_declaration.is_strict_mode(),
                function_declaration.parsing_insights());

            // iii. Perform ! env.InitializeBinding(fn, fo). NOTE: This step is replaced in section B.3.2.6.
            if (function_declaration.name_identifier()->is_local()) {
                auto& running_execution_context = vm.running_execution_context();
                auto number_of_registers = running_execution_context.executable->number_of_registers;
                auto number_of_constants = running_execution_context.executable->constants.size();
                running_execution_context.local(function_declaration.name_identifier()->local_variable_index() + number_of_registers + number_of_constants) = function;
            } else {
                VERIFY(is<DeclarativeEnvironment>(*environment));
                static_cast<DeclarativeEnvironment&>(*environment).initialize_or_set_mutable_binding({}, vm, function_declaration.name(), function);
            }
        }
    }));
}

// 16.1.7 GlobalDeclarationInstantiation ( script, env ), https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
ThrowCompletionOr<void> Program::global_declaration_instantiation(VM& vm, GlobalEnvironment& global_environment) const
{
    auto& realm = *vm.current_realm();

    // 1. Let lexNames be the LexicallyDeclaredNames of script.
    // 2. Let varNames be the VarDeclaredNames of script.
    // 3. For each element name of lexNames, do
    TRY(for_each_lexically_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
        auto const& name = identifier.string();

        // a. If env.HasVarDeclaration(name) is true, throw a SyntaxError exception.
        if (global_environment.has_var_declaration(name))
            return vm.throw_completion<SyntaxError>(ErrorType::TopLevelVariableAlreadyDeclared, name);

        // b. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
        if (global_environment.has_lexical_declaration(name))
            return vm.throw_completion<SyntaxError>(ErrorType::TopLevelVariableAlreadyDeclared, name);

        // c. Let hasRestrictedGlobal be ? env.HasRestrictedGlobalProperty(name).
        auto has_restricted_global = TRY(global_environment.has_restricted_global_property(name));

        // d. If hasRestrictedGlobal is true, throw a SyntaxError exception.
        if (has_restricted_global)
            return vm.throw_completion<SyntaxError>(ErrorType::RestrictedGlobalProperty, name);

        return {};
    }));

    // 4. For each element name of varNames, do
    TRY(for_each_var_declared_identifier([&](auto const& identifier) -> ThrowCompletionOr<void> {
        // a. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
        if (global_environment.has_lexical_declaration(identifier.string()))
            return vm.throw_completion<SyntaxError>(ErrorType::TopLevelVariableAlreadyDeclared, identifier.string());

        return {};
    }));

    // 5. Let varDeclarations be the VarScopedDeclarations of script.
    // 6. Let functionsToInitialize be a new empty List.
    Vector<FunctionDeclaration const&> functions_to_initialize;

    // 7. Let declaredFunctionNames be a new empty List.
    HashTable<FlyString> declared_function_names;

    // 8. For each element d of varDeclarations, in reverse List order, do

    TRY(for_each_var_function_declaration_in_reverse_order([&](FunctionDeclaration const& function) -> ThrowCompletionOr<void> {
        // a. If d is neither a VariableDeclaration nor a ForBinding nor a BindingIdentifier, then
        // i. Assert: d is either a FunctionDeclaration, a GeneratorDeclaration, an AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration.
        // Note: This is checked in for_each_var_function_declaration_in_reverse_order.

        // ii. NOTE: If there are multiple function declarations for the same name, the last declaration is used.

        // iii. Let fn be the sole element of the BoundNames of d.

        // iv. If fn is not an element of declaredFunctionNames, then
        if (declared_function_names.set(function.name()) != AK::HashSetResult::InsertedNewEntry)
            return {};

        // 1. Let fnDefinable be ? env.CanDeclareGlobalFunction(fn).
        auto function_definable = TRY(global_environment.can_declare_global_function(function.name()));

        // 2. If fnDefinable is false, throw a TypeError exception.
        if (!function_definable)
            return vm.throw_completion<TypeError>(ErrorType::CannotDeclareGlobalFunction, function.name());

        // 3. Append fn to declaredFunctionNames.
        // Note: Already done in step iv. above.

        // 4. Insert d as the first element of functionsToInitialize.
        // NOTE: Since prepending is much slower, we just append
        //       and iterate in reverse order in step 16 below.
        functions_to_initialize.append(function);
        return {};
    }));

    // 9. Let declaredVarNames be a new empty List.
    HashTable<FlyString> declared_var_names;

    // 10. For each element d of varDeclarations, do
    TRY(for_each_var_scoped_variable_declaration([&](Declaration const& declaration) {
        // a. If d is a VariableDeclaration, a ForBinding, or a BindingIdentifier, then
        // Note: This is done in for_each_var_scoped_variable_declaration.

        // i. For each String vn of the BoundNames of d, do
        return declaration.for_each_bound_identifier([&](auto const& identifier) -> ThrowCompletionOr<void> {
            auto const& name = identifier.string();
            // 1. If vn is not an element of declaredFunctionNames, then
            if (declared_function_names.contains(name))
                return {};

            // a. Let vnDefinable be ? env.CanDeclareGlobalVar(vn).
            auto var_definable = TRY(global_environment.can_declare_global_var(name));

            // b. If vnDefinable is false, throw a TypeError exception.
            if (!var_definable)
                return vm.throw_completion<TypeError>(ErrorType::CannotDeclareGlobalVariable, name);

            // c. If vn is not an element of declaredVarNames, then
            // i. Append vn to declaredVarNames.
            declared_var_names.set(name);
            return {};
        });
    }));

    // 11. NOTE: No abnormal terminations occur after this algorithm step if the global object is an ordinary object. However, if the global object is a Proxy exotic object it may exhibit behaviours that cause abnormal terminations in some of the following steps.
    // 12. NOTE: Annex B.3.2.2 adds additional steps at this point.

    // 12. Let strict be IsStrict of script.
    // 13. If strict is false, then
    if (!m_is_strict_mode) {
        // a. Let declaredFunctionOrVarNames be the list-concatenation of declaredFunctionNames and declaredVarNames.
        // b. For each FunctionDeclaration f that is directly contained in the StatementList of a Block, CaseClause, or DefaultClause Contained within script, do
        TRY(for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) -> ThrowCompletionOr<void> {
            // i. Let F be StringValue of the BindingIdentifier of f.
            auto function_name = function_declaration.name();

            // ii. If replacing the FunctionDeclaration f with a VariableStatement that has F as a BindingIdentifier would not produce any Early Errors for script, then
            // Note: This step is already performed during parsing and for_each_function_hoistable_with_annexB_extension so this always passes here.

            // 1. If env.HasLexicalDeclaration(F) is false, then
            if (global_environment.has_lexical_declaration(function_name))
                return {};

            // a. Let fnDefinable be ? env.CanDeclareGlobalVar(F).
            auto function_definable = TRY(global_environment.can_declare_global_function(function_name));
            // b. If fnDefinable is true, then

            if (!function_definable)
                return {};

            // i. NOTE: A var binding for F is only instantiated here if it is neither a VarDeclaredName nor the name of another FunctionDeclaration.

            // ii. If declaredFunctionOrVarNames does not contain F, then

            if (!declared_function_names.contains(function_name) && !declared_var_names.contains(function_name)) {
                // i. Perform ? env.CreateGlobalVarBinding(F, false).
                TRY(global_environment.create_global_var_binding(function_name, false));

                // ii. Append F to declaredFunctionOrVarNames.
                declared_function_names.set(function_name);
            }

            // iii. When the FunctionDeclaration f is evaluated, perform the following steps in place of the FunctionDeclaration Evaluation algorithm provided in 15.2.6:
            //     i. Let genv be the running execution context's VariableEnvironment.
            //     ii. Let benv be the running execution context's LexicalEnvironment.
            //     iii. Let fobj be ! benv.GetBindingValue(F, false).
            //     iv. Perform ? genv.SetMutableBinding(F, fobj, false).
            //     v. Return unused.
            function_declaration.set_should_do_additional_annexB_steps();

            return {};
        }));

        // We should not use declared function names below here anymore since these functions are not in there in the spec.
        declared_function_names.clear();
    }

    // 13. Let lexDeclarations be the LexicallyScopedDeclarations of script.
    // 14. Let privateEnv be null.
    PrivateEnvironment* private_environment = nullptr;

    // 15. For each element d of lexDeclarations, do
    TRY(for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        // a. NOTE: Lexically declared names are only instantiated here but not initialized.
        // b. For each element dn of the BoundNames of d, do
        return declaration.for_each_bound_identifier([&](auto const& identifier) -> ThrowCompletionOr<void> {
            auto const& name = identifier.string();
            // i. If IsConstantDeclaration of d is true, then
            if (declaration.is_constant_declaration()) {
                // 1. Perform ? env.CreateImmutableBinding(dn, true).
                TRY(global_environment.create_immutable_binding(vm, name, true));
            }
            // ii. Else,
            else {
                // 1. Perform ? env.CreateMutableBinding(dn, false).
                TRY(global_environment.create_mutable_binding(vm, name, false));
            }

            return {};
        });
    }));

    // 16. For each Parse Node f of functionsToInitialize, do
    // NOTE: We iterate in reverse order since we appended the functions
    //       instead of prepending. We append because prepending is much slower
    //       and we only use the created vector here.
    for (auto& declaration : functions_to_initialize.in_reverse()) {
        // a. Let fn be the sole element of the BoundNames of f.
        // b. Let fo be InstantiateFunctionObject of f with arguments env and privateEnv.
        auto function = ECMAScriptFunctionObject::create(realm, declaration.name(), declaration.source_text(), declaration.body(), declaration.parameters(), declaration.function_length(), declaration.local_variables_names(), &global_environment, private_environment, declaration.kind(), declaration.is_strict_mode(),
            declaration.parsing_insights());

        // c. Perform ? env.CreateGlobalFunctionBinding(fn, fo, false).
        TRY(global_environment.create_global_function_binding(declaration.name(), function, false));
    }

    // 17. For each String vn of declaredVarNames, do
    for (auto& var_name : declared_var_names) {
        // a. Perform ? env.CreateGlobalVarBinding(vn, false).
        TRY(global_environment.create_global_var_binding(var_name, false));
    }

    // 18. Return unused.
    return {};
}

ModuleRequest::ModuleRequest(FlyString module_specifier_, Vector<ImportAttribute> attributes)
    : module_specifier(move(module_specifier_))
    , attributes(move(attributes))
{
    // Perform step 10.e. from EvaluateImportCall, https://tc39.es/proposal-import-attributes/#sec-evaluate-import-call
    // or step 2. from WithClauseToAttributes, https://tc39.es/proposal-import-attributes/#sec-with-clause-to-attributes
    // e. / 2. Sort assertions by the code point order of the [[Key]] of each element.
    // NOTE: This sorting is observable only in that hosts are prohibited from distinguishing among assertions by the order they occur in.
    quick_sort(this->attributes, [](ImportAttribute const& lhs, ImportAttribute const& rhs) {
        return lhs.key < rhs.key;
    });
}

ByteString SourceRange::filename() const
{
    return code->filename().to_byte_string();
}

NonnullRefPtr<CallExpression> CallExpression::create(SourceRange source_range, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens)
{
    return ASTNodeWithTailArray::create<CallExpression>(arguments.size(), move(source_range), move(callee), arguments, invocation_style, inside_parens);
}

NonnullRefPtr<NewExpression> NewExpression::create(SourceRange source_range, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens)
{
    return ASTNodeWithTailArray::create<NewExpression>(arguments.size(), move(source_range), move(callee), arguments, invocation_style, inside_parens);
}

NonnullRefPtr<FunctionParameters> FunctionParameters::empty()
{
    static auto empty = adopt_ref(*new FunctionParameters({}));
    return empty;
}

}
