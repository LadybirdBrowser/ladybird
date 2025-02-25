# Ladybird C++ coding style

For low-level styling (spaces, parentheses, brace placement, etc), all code should follow the format specified in `.clang-format` in the project root.

**Important: Make sure you use `clang-format` version 19!**

This document describes the coding style used for C++ code in the Ladybird Browser project. All new code should conform to this style.

We'll definitely be tweaking and amending this over time, so let's consider it a living document. :)


### Names

A combination of CamelCase, snake\_case, and SCREAMING\_CASE:

- Use CamelCase (Capitalize the first letter, including all letters in an acronym) in a class, struct, or namespace name
- Use snake\_case (all lowercase, with underscores separating words) for variable and function names
- Use SCREAMING\_CASE for constants (both global and static member variables)

###### Right:

```cpp
struct Entry;
size_t buffer_size;
class FileDescriptor;
String absolute_path();
```

###### Wrong:

```cpp
struct data;
size_t bufferSize;
class Filedescriptor;
String MIME_Type();
```

When implementing spec algorithms and other constructs that a spec explicitly names, prefer closely matching the same names the spec uses, whenever possible.

Given the construct at https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-being-missing which has the literal name _“Suffering from being missing”_ in the spec, for example:

###### Right:

```c++
bool HTMLInputElement::suffering_from_being_missing(); // exactly matches the spec naming
```

###### Wrong:

```c++
bool HTMLInputElement::has_missing_constraint(); // arbitrarily differs from spec naming
```

Use full words, except in the rare case where an abbreviation would be more canonical and easier to understand.

###### Right:

```cpp
size_t character_size;
size_t length;
short tab_index; // More canonical.
```

###### Wrong:

```cpp
size_t char_size;
size_t len;
short tabulation_index; // Goofy.
```

Data members in C++ classes should be private. Static data members should be prefixed by "s\_". Other data members should be prefixed by "m\_". Global variables should be prefixed by "g\_".

###### Right:

```cpp
class String {
public:
    ...

private:
    int m_length { 0 };
};
```

###### Wrong:

```cpp
class String {
public:
    ...

    int length { 0 };
};
```

Precede setters with the word "set". Use bare words for getters. Setter and getter names should match the names of the variables being set/gotten.

###### Right:

```cpp
void set_count(int); // Sets m_count.
int count() const; // Returns m_count.
```

###### Wrong:

```cpp
void set_count(int); // Sets m_the_count.
int get_count() const; // Returns m_the_count.
```

Precede getters that return values through out arguments with the word "get".

###### Right:

```cpp
void get_filename_and_inode_id(String&, InodeIdentifier&) const;
```

###### Wrong:

```cpp
void filename_and_inode_id(String&, InodeIdentifier&) const;
```

Use descriptive verbs in function names.

###### Right:

```cpp
bool convert_to_ascii(short*, size_t);
```

###### Wrong:

```cpp
bool to_ascii(short*, size_t);
```

When there are two getters for a variable, and one of them automatically makes sure the requested object is instantiated, prefix that getter function with `ensure_`. As it ensures that an object is created, it should consequently also return a reference, not a pointer.

###### Right:

```cpp
Inode* inode();
Inode& ensure_inode();
```

###### Wrong:

```cpp
Inode& inode();
Inode* ensure_inode();
```

Leave meaningless variable names out of function declarations. A good rule of thumb is if the parameter type name contains the parameter name (without trailing numbers or pluralization), then the parameter name isn't needed. Usually, there should be a parameter name for bools, strings, and numerical types.

###### Right:

```cpp
void set_count(int);

void do_something(Context*);
```

###### Wrong:

```cpp
void set_count(int count);

void do_something(Context* context);
```

Prefer enums to bools on function parameters if callers are likely to be passing constants, since named constants are easier to read at the call site. An exception to this rule is a setter function, where the name of the function already makes clear what the boolean is.

###### Right:

```cpp
do_something(something, AllowFooBar::Yes);
paint_text_with_shadows(context, ..., text_stroke_width > 0, is_horizontal());
set_resizable(false);
```

###### Wrong:

```cpp
do_something(something, false);
set_resizable(NotResizable);
```

Enum members should use InterCaps with an initial capital letter.

Prefer `const` to `#define`. Prefer inline functions to macros.

`#defined` constants should use all uppercase names with words separated by underscores.

Use `#pragma once` instead of `#define` and `#ifdef` for header guards.

###### Right:

```cpp
// MyClass.h
#pragma once
```

###### Wrong:

```cpp
// MyClass.h
#ifndef MyClass_h
#define MyClass_h
```

### Other Punctuation

Constructors for C++ classes should initialize their members using C++ initializer syntax. Each member (and superclass) should be indented on a separate line, with the colon or comma preceding the member on that line. Prefer initialization at member definition whenever possible.

###### Right:

```cpp
class MyClass {
    ...
    Document* m_document { nullptr };
    int m_my_member { 0 };
};

MyClass::MyClass(Document* document)
    : MySuperClass()
    , m_document(document)
{
}

MyOtherClass::MyOtherClass()
    : MySuperClass()
{
}
```

###### Wrong:

```cpp
MyClass::MyClass(Document* document) : MySuperClass()
{
    m_myMember = 0;
    m_document = document;
}

MyClass::MyClass(Document* document) : MySuperClass()
    : m_my_member(0) // This should be in the header.
{
    m_document = document;
}

MyOtherClass::MyOtherClass() : MySuperClass() {}
```

Prefer index or range-for over iterators in Vector iterations for terse, easier-to-read code.

###### Right:

```cpp
for (auto& child : children)
    child->do_child_thing();
```


#### OK:

```cpp
for (int i = 0; i < children.size(); ++i)
    children[i]->do_child_thing();
```

###### Wrong:

```cpp
for (auto it = children.begin(); it != children.end(); ++it)
    (*it)->do_child_thing();
```

### Pointers and References

Both pointer types and reference types should be written with no space between the type name and the `*` or `&`.

An out argument of a function should be passed by reference except rare cases where it is optional in which case it should be passed by pointer.

###### Right:

```cpp
void MyClass::get_some_value(OutArgumentType& out_argument) const
{
    out_argument = m_value;
}

void MyClass::do_something(OutArgumentType* out_argument) const
{
    do_the_thing();
    if (out_argument)
        *out_argument = m_value;
}
```

###### Wrong:

```cpp
void MyClass::get_some_value(OutArgumentType* outArgument) const
{
    *out_argument = m_value;
}
```

### "using" Statements

In header files in the AK sub-library, however, it is acceptable to use "using" declarations at the end of the file to import one or more names in the AK namespace into the global scope.

###### Right:

```cpp
// AK/Vector.h

namespace AK {

} // namespace AK

using AK::Vector;
```

###### Wrong:

```cpp
// AK/Vector.h

namespace AK {

} // namespace AK

using namespace AK;
```

###### Wrong:

```cpp
// runtime/Object.h

namespace AK {

} // namespace AK

using AK::SomethingOrOther;
```

In C++ implementation files, do not use "using" declarations of any kind to import names in the standard template library. Directly qualify the names at the point they're used instead.

###### Right:

```cpp
// File.cpp

std::swap(a, b);
c = std::numeric_limits<int>::max()
```

###### Wrong:

```cpp
// File.cpp

using std::swap;
swap(a, b);
```

###### Wrong:

```cpp
// File.cpp

using namespace std;
swap(a, b);
```

### Types

Omit "int" when using "unsigned" modifier. Do not use "signed" modifier. Use "int" by itself instead.

###### Right:

```cpp
unsigned a;
int b;
```

###### Wrong:

```cpp
unsigned int a; // Doesn't omit "int".
signed b; // Uses "signed" instead of "int".
signed int c; // Doesn't omit "signed".
```

### Classes

For types with methods, prefer `class` over `struct`.

* For classes, make public getters and setters, keep members private with `m_` prefix.
* For structs, let everything be public and skip the `m_` prefix.

###### Right:

```cpp
struct Thingy {
    String name;
    int frob_count { 0 };
};

class Doohickey {
public:
    String const& name() const { return m_name; }
    int frob_count() const { return m_frob_count; }

    void jam();

private:
    String m_name;
    int m_frob_count { 0 };
}
```

###### Wrong:

```cpp
struct Thingy {
public:
    String m_name;
    int frob_count() const { return m_frob_count; }

private:
    int m_frob_count { 0 };
}

class Doohickey {
public:
    String const& name() const { return this->name; }

    void jam();

    String name;
    int frob_count { 0 };
};
```

Use a constructor to do an implicit conversion when the argument is reasonably thought of as a type conversion and the type conversion is fast. Otherwise, use the explicit keyword or a function returning the type. This only applies to single argument constructors.

###### Right:

```cpp
class LargeInt {
public:
    LargeInt(int);
...

class Vector {
public:
    explicit Vector(int size); // Not a type conversion.
    Vector create(Array); // Costly conversion.
...

```

###### Wrong:

```cpp
class Task {
public:
    Task(ExecutionContext&); // Not a type conversion.
    explicit Task(); // No arguments.
    explicit Task(ExecutionContext&, Other); // More than one argument.
...
```

### Singleton pattern

Use a static member function named "the()" to access the instance of the singleton.

###### Right:

```cpp
class UniqueObject {
public:
    static UniqueObject& the();
...
```

###### Wrong:

```cpp
class UniqueObject {
public:
    static UniqueObject& shared();
...
```

###### Wrong:

```cpp
class UniqueObject {
...
};

UniqueObject& my_unique_object(); // Free function.
```

### Comments

Comments should be written using `//` and not `/* */`, except for the copyright notice.

Write comments as proper sentences starting with a capital letter and ending with a period (or other punctuation).
One exception may be end of line comments like this: `if (x == y) // false for NaN`
Another exception is comments copied from specifications. These should be quoted verbatim, and not modified except for
wrapping or to insert necessary punctuation such as adding `**` when a number is raised to a power, as this is often
done using elements like `<sup>` which do not appear in the copied text.

Please wrap long comments onto multiple lines so that they are easier to read. Generally, 120 characters is a good width
to aim for.

Use FIXME: (without attribution) to denote items that need to be addressed in the future.
TODO: (without attribution) is also permitted.

###### Right:

```cpp
draw_jpg(); // FIXME: Make this code handle jpg in addition to the png support.
```

###### OK:

```cpp
draw_jpg(); // TODO: Make this code handle jpg in addition to the png support.
```

###### Wrong:

```cpp
draw_jpg(); // FIXME(joe): Make this code handle jpg in addition to the png support.
```

Explain *why* the code does something. The code itself should already say what is happening.

###### Right:

```cpp
i++; // Go to the next page.
```

```cpp
// Let users toggle the advice functionality by clicking on catdog.
catdog_widget.on_click = [&] {
    if (advice_timer->is_active())
        advice_timer->stop();
    else
        advice_timer->start();
};
```

###### Even better:

```cpp
page_index++;
```

###### Wrong:

```cpp
i++; // Increment i.
```

```cpp
// If the user clicks, toggle the timer state.
catdog_widget.on_click = [&] {
    if (advice_timer->is_active())
        advice_timer->stop();
    else
        advice_timer->start();
};
```

### Overriding Virtual Methods

The declaration of a virtual method inside a class must be declared with the `virtual` keyword. All subclasses of that class must also specify either the `override` keyword when overriding the virtual method, or the `final` keyword when overriding the virtual method and requiring that no further subclasses can override it.

###### Right:

```cpp
class Person {
public:
    virtual String description() { ... };
}

class Student : public Person {
public:
    virtual String description() override { ... }; // This is correct because it contains both the "virtual" and "override" keywords to indicate that the method is overridden.
}

```

```cpp
class Person {
public:
    virtual String description() { ... };
}

class Student : public Person {
public:
    virtual String description() final { ... }; // This is correct because it contains both the "virtual" and "final" keywords to indicate that the method is overridden and that no subclasses of "Student" can override "description".
}

```

###### Wrong:

```cpp
class Person {
public:
    virtual String description() { ... };
}

class Student : public Person {
public:
    String description() override { ... }; // This is incorrect because it uses only the "override" keyword to indicate that the method is virtual. Instead, it should use both the "virtual" and "override" keywords.
}
```

```cpp
class Person {
public:
    virtual String description() { ... };
}

class Student : public Person {
public:
    String description() final { ... }; // This is incorrect because it uses only the "final" keyword to indicate that the method is virtual and final. Instead, it should use both the "virtual" and "final" keywords.
}
```

```cpp
class Person {
public:
    virtual String description() { ... };
}

class Student : public Person {
public:
    virtual String description() { ... }; // This is incorrect because it uses only the "virtual" keyword to indicate that the method is overridden.
}
```

### Const placement

Use "east const" style where `const` is written on the right side of the type being qualified. See [this article](https://mariusbancila.ro/blog/2018/11/23/join-the-east-const-revolution/) for more information about east const.

###### Right:

```cpp
Salt const& m_salt;
```

###### Wrong:

```cpp
const Salt& m_salt;
```

### Casts

Before you consider a cast, please see if your problem can be solved another way that avoids the visual clutter.

- Integer constants can be specified to have (some) specific sizes with postfixes like `u, l, ul` etc. The same goes for single-precision floating-point constants with `f`.
- Working with smaller-size integers in arithmetic expressions is hard because of [implicit promotion](https://wiki.sei.cmu.edu/confluence/display/c/INT02-C.+Understand+integer+conversion+rules). Generally, it is fine to use `int` and other "large" types in local variables, and possibly cast at the end.
- If you `const_cast`, _really_ consider whether your APIs need to be adjusted in terms of their constness. Does the member function you're writing actually make sense to be `const`?
- If you do checked casts between base and derived types, also consider your APIs. For example: Does the function being called actually need to receive the more general type or is it fine with the more specialized type?

If you _do_ need to cast: **Don't use C-style casts**. The C-style cast has [complex behavior](https://en.cppreference.com/w/c/language/cast) that is undesired in many instances. Be aware of what sort of type conversion the code is trying to achieve, and use the appropriate (!) C++ cast operator, like `static_cast`, `reinterpret_cast`, `bit_cast`, `dynamic_cast` etc.

There is a single exception to this rule: marking a function parameter as used with `(void)parameter;`.

###### Right:
```cpp
MyParentClass& object = get_object();
// Verify the type...
MyChildClass& casted = static_cast<MyChildClass&>(object);
```

```cpp
// AK::Atomic::exchange()

alignas(T) u8 buffer[sizeof(T)];
T* ret = reinterpret_cast<T*>(buffer);
```

```cpp
// SeekableStream::tell()

// Seek with 0 and SEEK_CUR does not modify anything despite the const_cast,
// so it's safe to do this.
return const_cast<SeekableStream*>(this)->seek(0, SeekMode::FromCurrentPosition);
```

###### Wrong:
```cpp
// These should be static_cast.
size_t mask_length = (size_t)((u8)-1) + 1;
```

```cpp
// This should be reinterpret_cast.
return (u8 const*)string.characters_without_null_termination();
```

### Omission of curly braces from statement blocks

Curly braces may only be omitted from `if`/`else`/`for`/`while`/etc. statement blocks if the body is a single line.

Additionally, if any body of a connected if/else statement requires curly braces according to this rule, all of them do.

###### Right:
```cpp
if (condition)
    foo();
```

```cpp
if (condition) {
    foo();
    bar();
}
```

```cpp
if (condition) {
    foo();
} else if (condition) {
    bar();
    baz();
} else {
    qux();
}
```

```cpp
for (size_t i = i; condition; ++i) {
    if (other_condition)
        foo();
}
```

##### OK:

```cpp
if (condition) {
    foo();
}
```

###### Wrong:

```cpp
if (condition)
    // There is a comment here.
    foo();
```

```cpp
if (condition)
    foo();
else {
    bar();
    baz();
} else
    qux();
```

```cpp
for (size_t i = i; condition; ++i)
    if (other_condition)
        foo();
```
