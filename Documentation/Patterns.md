# Coding Patterns

## Introduction

Over time numerous reoccurring patterns have emerged from or were adopted by
the Ladybird code base. This document aims to track and describe them, so they
can be propagated further and the code base can be kept consistent. 

## `TRY(...)` Error Handling

The `TRY(..)` macro is used for error propagation in the Ladybird code base.
The goal being to reduce the amount of boiler plate error code required to
properly handle and propagate errors throughout the code base. 

Any code surrounded by `TRY(..)` will attempt to be executed, and any error
will immediately be returned from the function. If no error occurs then the
result of the contents of the TRY will be the result of the macro's execution. 

### Examples:

Example from LibGfx:

```cpp
#include <AK/Try.h>

... snip ...

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create_shareable(BitmapFormat format, IntSize size, int scale_factor)
{
    if (size_would_overflow(format, size, scale_factor))
        return Error::from_string_literal("Gfx::Bitmap::create_shareable size overflow");

    auto const pitch = minimum_pitch(size.width() * scale_factor, format);
    auto const data_size = size_in_bytes(pitch, size.height() * scale_factor);

    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(round_up_to_power_of_two(data_size, PAGE_SIZE)));
    auto bitmap = TRY(Bitmap::create_with_anonymous_buffer(format, buffer, size, scale_factor, {}));
    return bitmap;
}
```

Note: Our `TRY(...)` macro functions similarly to the `?` [operator in rust](https://doc.rust-lang.org/book/ch09-02-recoverable-errors-with-result.html#a-shortcut-for-propagating-errors-the--operator).

## `MUST(...)` Error Handling

The `MUST(...)` macro is similar to `TRY(...)` except the macro enforces that
the code run inside the macro must succeed, otherwise we assert.

Note that `MUST(...)` should not be used as a replacement for `TRY(...)` in cases where error propagation is not (currently) possible.
Instead, the `release_value_but_fixme_should_propagate_errors()` method of `ErrorOr<>` should be used to retrieve the value
and to mark the location for future improvement. `MUST(...)` is reserved for cases where we determine through other circumstances that it
should not be possible for the code inside the macro to fail or if a failure is serious enough that the program _needs_ to crash.

### Example:

```cpp
#include <AK/Vector.h>

... snip ...

ErrorOr<void> insert_one_to_onehundred(Vector<int>& vector)
{
    TRY(vector.try_ensure_capacity(vector.size() + 100));

    for (int i = 1; i <= 100; i++) {
        // We previously made sure that we allocated enough space, so the append operation shouldn't ever fail.
        MUST(vector.try_append(i));
    }

    return {};
}
```

## Fallible Constructors

The usual C++ constructors are incompatible with Ladybird's method of handling errors,
as potential errors are passed using the `ErrorOr` return type. As a replacement, classes
that require fallible operations during their construction define a static function that
is fallible instead.

This fallible function (which should usually be named `create`) will handle any errors while
preparing arguments for the internal constructor and run any required fallible operations after
the object has been initialized. The resulting object is then returned as `ErrorOr<T>` or
`ErrorOr<NonnullOwnPtr<T>>`.

### Example:

```cpp
class Decompressor {
public:
    static ErrorOr<NonnullOwnPtr<Decompressor>> create(NonnullOwnPtr<Core::Stream::Stream> stream)
    {
        auto buffer = TRY(CircularBuffer::create_empty(32 * KiB));
        auto decompressor = TRY(adopt_nonnull_own_or_enomem(new (nothrow) Decompressor(move(stream), move(buffer))));
        TRY(decompressor->initialize_settings_from_header());
        return decompressor;
    }

... snip ...

private:
    Decompressor(NonnullOwnPtr<Core::Stream::Stream> stream, CircularBuffer buffer)
        : m_stream(move(stream))
        , m_buffer(move(buffer))
    {
    }

    CircularBuffer m_buffer;
    NonnullOwnPtr<Core::Stream::Stream> m_stream;
}
```

## The `serenity_main(..)` program entry point

Ladybird has moved to a pattern where executables do not expose a normal C
main function. A `serenity_main(..)` is exposed instead. The main reasoning
is that the `Main::Arguments` struct can provide arguments in a more idiomatic
way that fits with the Ladybird's internal API surface area. The ErrorOr<int> likewise
allows the program to propagate errors seamlessly with the `TRY(...)` macro,
avoiding a significant amount of clunky C style error handling.

These executables are then linked with the `LibMain` library, which will link in
the normal C `int main(int, char**)` function which will call into the programs
`serenity_main(..)` on program startup.

The creation of the pattern was documented in the following video:
[OS hacking: A better main() for SerenityOS C++ programs](https://www.youtube.com/watch?v=5PciKJW1rUc)

### Examples:

A function `main(..)` would normally look something like:

```cpp
int main(int argc, char** argv)
{
   return 0;
}
```

Instead, `serenity_main(..)` is defined like this:

```cpp
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    return 0; 
}
```

## Intrusive Lists

[Intrusive lists](https://www.data-structures-in-practice.com/intrusive-linked-lists/) are used in some specific cases
in Ladybird. A data structure is said to be
"intrusive" when each element holds the metadata that tracks the
element's membership in the data structure. In the case of a list, this
means that every element in an intrusive linked list has a node embedded
inside it. The main advantage of intrusive
data structures is you don't need to worry about handling out of memory (OOM)
on insertion into the data structure. This means error handling code is
much simpler than say, using a `Vector` in environments that need to be durable
to OOM.

The common pattern for declaring an intrusive list is to add the storage
for the intrusive list node as a private member. A public type alias is
then used to expose the list type to anyone who might need to create it.

## Static Assertions of the size of a type

It's a universal pattern to use `static_assert` to validate the size of a
type matches the author's expectations. Unfortunately when these assertions
fail they don't give you the values that actually caused the failure. This
forces one to go investigate by printing out the size, or checking it in a
debugger, etc. 

For this reason `AK::AssertSize` was added. It exploits the fact that the
compiler will emit template argument values for compiler errors to provide
debugging information. Instead of getting no information you'll get the actual
type sizes in your compiler error output.

Example Usage:

```cpp
#include <AK/StdLibExtras.h>

struct Empty { };

static_assert(AssertSize<Empty, 1>());
```

## String View Literals

`AK::StringView` support for `operator""sv` which is a special string literal operator that was added as of
[C++17 to enable `std::string_view` literals](https://en.cppreference.com/w/cpp/string/basic_string_view/operator%22%22sv).

```cpp
[[nodiscard]] ALWAYS_INLINE constexpr AK::StringView operator""sv(const char* cstring, size_t length)
{
    return AK::StringView(cstring, length);
}
```

This allows `AK::StringView` to be constructed from string literals with no runtime
cost to find the string length, and the data the `AK::StringView` points to will 
reside in the data section of the binary.

Example Usage:
```cpp
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibTest/TestCase.h>

TEST_CASE(string_view_literal_operator)
{
    StringView literal_view = "foo"sv;
    String test_string = "foo";

    EXPECT_EQ(literal_view.length(), test_string.length());
    EXPECT_EQ(literal_view, test_string);
}
```

## Source Location

C++20 added std::source_location, which lets you capture the
callers __FILE__ / __LINE__ / __FUNCTION__ etc as a default
argument to functions.
See: https://en.cppreference.com/w/cpp/utility/source_location

`AK::SourceLocation` is the implementation of this feature in
Ladybird's AK. It's become the idiomatic way to capture the location
when adding extra debugging instrumentation, without resorting to
littering the code with preprocessor macros.

To use it, you can add the `AK::SourceLocation` as a default argument
to any function, using `AK::SourceLocation::current()` to initialize the
default argument.

Example Usage:
```cpp
#include <AK/SourceLocation.h>
#include <AK/StringView.h>

static StringView example_fn(const SourceLocation& loc = SourceLocation::current())
{
    return loc.function_name();
}

int main(int, char**)
{
    return example_fn().length();
}
```

If you only want to only capture `AK::SourceLocation` data with a certain debug macro enabled, avoid
adding `#ifdef`'s to all functions which have the  `AK::SourceLocation` argument. Since SourceLocation
is just a simple struct, you can just declare an empty class which can be optimized away by the
compiler, and alias both to the same name.

Example Usage:

```cpp

#if LOCK_DEBUG
#    include <AK/SourceLocation.h>
#endif

#if LOCK_DEBUG
using LockLocation = SourceLocation;
#else
struct LockLocation {
    static constexpr LockLocation current() { return {}; }

private:
    constexpr LockLocation() = default;
};
#endif
```

## `type[]` vs. `Array<type>` vs. `Vector<type>` vs. `FixedArray<type>`

There are four "contiguous list" / array-like types, including C-style arrays themselves. They share a lot of their API, but their use cases are all slightly different, mostly relating to how they allocate their data.

Note that `Span<type>` differs from all of these types in that it provides a *view* on data owned by somebody else. The four types mentioned above all own their data, but they can provide `Span`'s which view all or part of their data. For APIs that aren't specific to the kind of list and don't need to handle resizing in any way, `Span` is a good choice.

* C-style arrays are generally discouraged (and this also holds for pointer+size-style arrays when passing them around). They are only used for the implementation of other collections or in specific circumstances.
* `Array` is a thin wrapper around C-style arrays similar to `std::array`, where the template arguments include the size of the array. It allocates its data inline, just as arrays do, and never does any dynamic allocations.
* `Vector` is similar to `std::vector` and represents a dynamic resizable array. For most basic use cases of lists, this is the go-to collection. It has an optional inline capacity (the second template argument) which will allocate inline as the name suggests, but this is not always used. If the contents outgrow the inline capacity, Vector will automatically switch to the standard out-of-line storage. This is allocated on the heap, and the space is automatically resized and moved when more (or less) space is needed.
* `FixedArray` is essentially a runtime-sized `Array`. It can't resize like `Vector`, but it's ideal for circumstances where the size is not known at compile time but doesn't need to change once the collection is initialized. `FixedArray` guarantees to not allocate or deallocate except for its constructor and destructor.
