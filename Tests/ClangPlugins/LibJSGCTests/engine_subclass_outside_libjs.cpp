/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/CyclicModule.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/TypedArray.h>

// expected-error@+1 {{ObjectSubclass derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class ObjectSubclass : public JS::Object {
};

// expected-error@+1 {{FunctionObjectSubclass derives from the engine type JS::FunctionObject, which only LibJS may subclass; use a host class instead}}
class FunctionObjectSubclass : public JS::FunctionObject {
};

// expected-error@+1 {{NativeFunctionSubclass derives from the engine type JS::NativeFunction, which only LibJS may subclass; use a host class instead}}
class NativeFunctionSubclass : public JS::NativeFunction {
};

// expected-error@+1 {{ArraySubclass derives from the engine type JS::Array, which only LibJS may subclass; use a host class instead}}
class ArraySubclass : public JS::Array {
};

// expected-error@+1 {{ErrorSubclass derives from the engine type JS::Error, which only LibJS may subclass; use a host class instead}}
class ErrorSubclass : public JS::Error {
};

// expected-error@+1 {{ModuleSubclass derives from the engine type JS::Module, which only LibJS may subclass; use a host class instead}}
class ModuleSubclass : public JS::Module {
};

// expected-error@+1 {{CyclicModuleSubclass derives from the engine type JS::CyclicModule, which only LibJS may subclass; use a host class instead}}
class CyclicModuleSubclass : public JS::CyclicModule {
};

// expected-error@+1 {{EnvironmentSubclass derives from the engine type JS::Environment, which only LibJS may subclass; use a host class instead}}
class EnvironmentSubclass : public JS::Environment {
};

// Engine types that derive from the ones above are engine types too.

// expected-error@+1 {{GlobalObjectSubclass derives from the engine type JS::GlobalObject, which only LibJS may subclass; use a host class instead}}
class GlobalObjectSubclass : public JS::GlobalObject {
};

// expected-error@+1 {{HostObjectSubclass derives from the engine type JS::HostObject, which only LibJS may subclass; use a host class instead}}
class HostObjectSubclass : public JS::HostObject {
};

// A subclass of a rejected class is reported only once, at the class that derives from the engine type.
class SubclassOfObjectSubclass : public ObjectSubclass {
};

// An engine type named through a type alias is still that engine type.
using AliasedEngineType = JS::Object;

// expected-error@+1 {{SubclassOfAliasedEngineType derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class SubclassOfAliasedEngineType : public AliasedEngineType {
};

// A class template whose base is an engine type for every argument is reported once, at the template.

// expected-error@+2 {{TemplateWithEngineBase derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
template<typename T>
class TemplateWithEngineBase : public JS::Object {
};

class SubclassOfTemplateWithEngineBase : public TemplateWithEngineBase<int> {
};

template class TemplateWithEngineBase<char>;

// expected-error@+2 {{TemplateWithDependentEngineBase derives from the engine type JS::TypedArray, which only LibJS may subclass; use a host class instead}}
template<typename T>
class TemplateWithDependentEngineBase : public JS::TypedArray<T> {
};

class SubclassOfTemplateWithDependentEngineBase : public TemplateWithDependentEngineBase<u8> {
};

// A class template that derives from its argument is reported for each instantiation that makes it an engine type.

// expected-error@+2 {{MixinOverBase<JS::Object> derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
template<typename BaseClass>
class MixinOverBase : public BaseClass {
};

// expected-note@+1 {{MixinOverBase<JS::Object> is instantiated here}}
class SubclassOfMixinOverEngineType : public MixinOverBase<JS::Object> {
};

// expected-error@+1 {{MixinOverBase<JS::NativeFunction> derives from the engine type JS::NativeFunction, which only LibJS may subclass; use a host class instead}}
template class MixinOverBase<JS::NativeFunction>;

// The member classes of an instantiation are not checked on their own, so the class derived from one is reported.
template<typename BaseClass>
struct TemplateWithMemberMixin {
    class Member : public BaseClass {
    };
};

// expected-error@+1 {{SubclassOfMemberMixin derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class SubclassOfMemberMixin : public TemplateWithMemberMixin<JS::Object>::Member {
};

// Instantiating an engine type that LibJS defines as a template is not subclassing it.
template class JS::TypedArray<i64>;

// Cells that are not engine objects may be defined anywhere.
class EmbedderCell : public JS::Cell {
    GC_CELL(EmbedderCell, JS::Cell);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_object);
    }

    GC::Ptr<JS::Object> m_object;
};

template<typename BaseClass>
class CellMixinOverBase : public BaseClass {
    GC_CELL(CellMixinOverBase, BaseClass);
};

class SubclassOfMixinOverCell : public CellMixinOverBase<JS::Cell> {
    GC_CELL(SubclassOfMixinOverCell, CellMixinOverBase<JS::Cell>);
};
