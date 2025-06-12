# String Formatting

Many places in Ladybird allow you to format strings, similar to `printf()`, for example `ByteString::formatted()`
, `StringBuilder::appendff()`, or `dbgln()`. These are checked at compile time to ensure the format string matches the
number of parameters. The syntax is largely based on
the [C++ `std::formatter` syntax](https://en.cppreference.com/w/cpp/utility/format/formatter#Standard_format_specification)
but there are some differences.

For basic usage, any occurrences of `{}` in the format string are replaced with the other arguments, converted to string
form, in order:

```c++
ByteString::formatted("Well, {} my {} friends!", "hello", 42) == "Well, hello my 42 friends!";
```

If you want to include a literal `{` in the output, use `{{`:

```c++
ByteString::formatted("{{ {}", "hello") == "{ hello";
```

You can refer to the arguments by index, if you want to repeat one or change the order:

```c++
ByteString::formatted("{2}{0}{1}", "a", "b", "c") == "cab";
```

To control how the arguments are formatted, add colon after the optional index, and then add format specifier
characters:

```c++
ByteString::formatted("{:.4}", "cool dude") == "cool";
ByteString::formatted("{0:.4}", "cool dude") == "cool";
```

## Format specifiers

In order, the format can contain:

- Fill character and alignment
- Sign
- `#` Hash
- `0` Zero
- Width
- Precision
- Type specifier

Each of these is optional. You can include any combination of them, but they must be in this order.

### Fill and alignment

This is an optional fill character, followed by an alignment. The fill character can be anything apart from `{` or `}`,
and is used to fill any space left when the input has fewer characters than the format requests. By default, it is a
space. (` `)

The alignment characters are:

- `<`: Align left.
- `>`: Align right.
- `^`: Align centered.

### Sign

- `+`: Always display a sign before the number.
- `-`: Display a sign for negative numbers only.
- (space): Display a sign for negative numbers, and a leading space for other numbers.

### Hash

`#` causes an "alternate form" to be used.

For integer types, this adds the number-base prefix after the sign:

- `0b` for binary.
- `0` for octal.
- `0x` for hexadecimal.

### Zero

`0` pads the number with leading zeros.

### Width and Precision

The width defines the minimum number of characters in the output. The precision is a `.` followed by a precision number,
which is used as the precision of floating-point numbers, or a maximum-width for string values.

Both the width and precision can be provided as a replacement field (`{}`, optionally including an argument index) which
allows you to use an integer argument instead of a hard-coded number.

### Type specifiers

| Type      | Effect                | Example output           |
|-----------|-----------------------|--------------------------|
| *nothing* | default format        | Anything! :^)            |
| b         | binary                | `110`, `0b000110`        |
| B         | binary uppercase      | `110`, `0B000110`        |
| d         | decimal               | `42`, `+0000042`         |
| o         | octal                 | `043`                    |
| x         | hexadecimal           | `ff0`, `0x00000ff0`      |
| X         | hexadecimal uppercase | `FF0`, `0X00000FF0`      |
| c         | character             | `a`                      |
| s         | string                | `well, hello friends!`   |
| p         | pointer               | `0xdeadc0de`             |
| f         | float                 | `1.234`, `-inf`          |
| a         | hex float             |                          |
| A         | hex float uppercase   |                          |
| hex-dump  | hexadecimal dump      | `fdfdfdfd`, `3030    00` |

Not all type specifiers are compatible with all input types, of course.

## Formatting custom types

You can provide a custom `AK::Formatter<Foo>` class to format `Foo` values. For the simplest case where you already have
a function that produces a string from your type, that would look like this:

```c++
template<>
struct AK::Formatter<Web::CSS::Selector> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Selector const& selector)
    {
        return Formatter<StringView>::format(builder, selector.serialize());
    }
};
```

More advanced formatters that make check for format-specifier flags can be written by referring to the fields
in `StandardFormatter` (which most `Formatter` classes extend).

## Detecting if a type can be formatted

The `AK::HasFormatter<T>` template has a boolean value representing whether `T` can be formatted.

The `FormatIfSupported<T>` makes use of this to return either the formatted value of `T`, or a series of `?`s if the
type cannot be formatted. For example:

```c++
// B has a Formatter defined, but A does not.
ByteString::formatted("{}", FormatIfSupported { A {} }) == "?";
ByteString::formatted("{}", FormatIfSupported { B {} }) == "B";
```
