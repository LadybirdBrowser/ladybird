ScopePusher – Violates SRP by Handling Both Scope Management and Identifier Declaration

## 1. ScopePusher: Scope Management & Identifier Declaration  
**File:** `Userland/Libraries/LibJS/ScopePusher.cpp`

**SRP Violation Description:**  
The `ScopePusher` class currently has two distinct responsibilities:
1. Managing different scope types (e.g., Function, Block, Catch)
2. Handling identifier declaration (e.g., storing `var`, `let`, `function` names and checking for conflicts)

Example:
```cpp
m_var_names.set(identifier->string());
m_bound_names.set(identifier->string());
m_catch_parameter_names.set(identifier->string());

This violates the Single Responsibility Principle because scope structure and identifier registration are separate concerns that may change independently.

Proposed Solution:
Split the class into two more focused components:

class ScopeContext {
    // Responsible only for scope structure and lifetime
};

class IdentifierRegistry {
    // Handles identifier storage, conflict resolution, and declaration rules
    void register_identifier(String name, DeclarationType type);
    bool is_declared(String name);
};

Benefits:

1. Improves modularity and separation of concerns

2. Easier to test identifier registration logic independently

3. Updates to JavaScript identifier rules (e.g., ESNext changes) only impact IdentifierRegistry, not the scope system



add_declaration() – Too Complex, Mixing Multiple Responsibilities

File: Userland/Libraries/LibJS/Bytecode/Generator.cpp

SRP Violation Description:
The add_declaration() function currently mixes several different responsibilities:

Validates the identifier

Handles grouping based on declaration type (var, let, const, function)

Differentiates between top-level and nested scopes

Determines appropriate storage strategy for the variable

This violates the Single Responsibility Principle, as each of these tasks could change independently and makes the function harder to understand or maintain.

Example:

if (declaration->is_lexical_declaration()) {
    // validation + set + store
} else if (!declaration->is_function_declaration()) {
    // validation + store in top-level node
} else {
    // function-specific logic
}

Proposed Solution:
Split the logic into smaller, more focused helper methods:

void handle_lexical_declaration(Declaration const&);
void handle_var_declaration(Declaration const&);
void handle_function_declaration(FunctionDeclaration const&);

Benefits:

1. Each method has a single purpose and is easier to read and understand

2. Code becomes easier to test in isolation

3. Prevents future bugs by avoiding entangled logic paths

4. Enhances maintainability as declaration rules evolve



add_declaration(): Mixed Responsibilities in Declaration Handling

File: Userland/Libraries/LibJS/Bytecode/Generator.cpp

SRP Violation Description:
The add_declaration() function is responsible for multiple tasks that should be separated:

Validating identifiers

Categorizing declarations (var, let, const, function)

Navigating scope levels (top-level vs. nested)

Deciding storage strategies based on declaration type

These combined responsibilities make the function hard to maintain, understand, and test.

Example:

if (declaration->is_lexical_declaration()) {
    // validation + set + store
} else if (!declaration->is_function_declaration()) {
    // validation + store in top-level node
} else {
    // function-specific logic
}

Proposed Solution:
Split the logic into smaller, more focused helper methods:

void handle_lexical_declaration(Declaration const&);
void handle_var_declaration(Declaration const&);
void handle_function_declaration(FunctionDeclaration const&);

Benefits:

1. Simplifies understanding and debugging

2. Each method becomes individually testable

3. Better separation of concerns

4. Easier to extend and maintain in the future



ScopePusher: Handles Both Scope Logic and Error Throwing

File: Userland/Libraries/LibJS/ScopePusher.cpp

SRP Violation Description:
The ScopePusher class directly handles error throwing when identifier redeclarations are detected:

throw_identifier_declared(name, declaration);

This means the class takes on two responsibilities:

Managing scope structure and identifier tracking

Managing error handling logic

Combining these breaks the Single Responsibility Principle (SRP), as throwing or reporting errors should be handled separately.

Proposed Solution:
Delegate error reporting to a dedicated class, such as DeclarationValidator or ErrorReporter:

if (!validator.validate_identifier(name, declaration))
    error_reporter.report_redeclaration(name, declaration);

Benefits:

1. Scope logic and error-handling are cleanly separated

2. Error strategies (e.g., logging, throwing, collecting) become easily customizable

3. Improves testability and reusability of each component



ScopePusher: Constructor Handles Excessive Conditional Logic

File: Userland/Libraries/LibJS/ScopePusher.cpp

SRP Violation Description:
The constructor of ScopePusher is responsible for too many tasks:

1.Deciding which node to assign

2.Setting the parent scope

3.Determining whether this is a top-level scope

4.Inheriting values from the parent if not top-level

Example:

if (!node)
    m_node = m_parent_scope->m_node;
else
    m_node = node;

This violates SRP, as object initialization logic and construction control flow are mixed together.

Proposed Solution:
Use factory methods (e.g., create_for_block(), create_for_function()) to separate configuration logic from the constructor itself:

ScopePusher::ScopePusher(...) 
    : m_parser(...), m_type(...) 
{ 
    // Pure assignment only
}

static ScopePusher create_for_block(...) {
    ScopePusher s(...);
    s.configure_block_scope(...);
    return s;
}

Benefits:

1.Constructor remains clean and predictable

2.Adding new scope types won’t bloat the constructor

3.Improves readability and separation of concerns

4. Easier to maintain and extend



 ScopeType Enum Centralizes All Scope Kinds

File: Userland/Libraries/LibJS/ScopeType.h

SRP Violation Description:
The ScopeType enum defines all scope types in a single centralized structure, like:

enum class ScopeType {
    Function,
    Program,
    Block,
    ForLoop,
    With,
    Catch,
    ClassStaticInit,
    ClassField,
    ClassDeclaration,
};

This violates SRP because each scope type in JavaScript has its own distinct rules and responsibilities (e.g., hoisting, variable lifetimes, closures, etc.). Centralizing all of them in a flat enum mixes unrelated logic into one place, making changes more fragile.

Proposed Solution:
Refactor using class hierarchy or composition where each scope type is a separate class derived from a base class:

class BaseScope {
    // Common scope behavior
};

class FunctionScope : public BaseScope {
    // Rules specific to functions
};

class BlockScope : public BaseScope {
    // Block-scoped rules (let, const)
};

// etc.

Benefits:

1. Each scope encapsulates its own logic and rules

2. Easier to extend with new scope types (e.g., PrivateFieldScope in future ECMAScript versions)

3. Improves modularity and testability

4. Localizes the impact of changes – modifying one scope type won’t break others

