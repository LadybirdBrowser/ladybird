# Lagom

The Serenity C++ libraries, for other Operating Systems.

## About

If you want to bring the comfortable Serenity classes with you to another system, look no further. This is basically a "port" of the `AK` and `LibCore` libraries to generic \*nix systems.

*Lagom* is a Swedish word that means "just the right amount." ([Wikipedia](https://en.wikipedia.org/wiki/Lagom))

Lagom is used by the Serenity project in the following ways:

- [Build tools](./Tools) required to build Serenity itself using Serenity's own C++ libraries are in Lagom.
- [Unit tests](../../Documentation/Testing.md) in CI are built using the Lagom build for host systems to ensure portability.
- [Continuous fuzzing](#fuzzing-on-oss-fuzz) is done with the help of OSS-fuzz using the Lagom build.
- [The Ladybird browser](../../README.md) uses Lagom to provide LibWeb and LibJS for non-Serenity systems.
- [ECMA 262 spec tests](https://ladybirdbrowser.github.io/libjs-website/test262) for LibJS are run per-commit and tracked on [LibJS website](https://ladybirdbrowser.github.io/libjs-website/).
- [Wasm spec tests](https://ladybirdbrowser.github.io/libjs-website/wasm) for LibWasm are run per-commit and tracked on [LibJS website](https://ladybirdbrowser.github.io/libjs-website/).

## Using Lagom in an External Project
It is possible to use Lagom for your own projects outside of Serenity too!

An example of this in use can be found in the [LibJS test262 runner](https://github.com/SerenityOS/libjs-test262).

To implement this yourself:
- Download a copy of [SerenityOS/libjs-test262/cmake/FetchLagom.cmake](https://github.com/SerenityOS/libjs-test262/blob/7832c333c1504eecf1c5f9e4247aa6b34a52a3be/cmake/FetchLagom.cmake) and place it wherever you wish
- In your root `CMakeLists.txt`, add the following commands:
  ```cmake
  include(FetchContent)
  include(cmake/FetchLagom.cmake) # If you've placed the file downloaded above differently, be sure to reflect that in this command :^)
  ```
- In addition, you will need to also add some compile options that Serenity uses to ensure no warnings or errors:
  ```cmake
  add_compile_options(-Wno-literal-suffix) # AK::StringView defines operator""sv, which GCC complains does not have an underscore.
  add_compile_options(-fno-gnu-keywords)   # JS::Value has a method named typeof, which also happens to be a GNU keyword.
  ```

Now, you can link against Lagom libraries.

Things to keep in mind:
- You should prefer to use a library's `Lagom::` alias when linking
  - Example: `Lagom::Core` vs `LibCore`
- If your application has name clashes with any names in AK, you may have to define `USING_AK_GLOBALLY=0` for the files that have visibility to both sets of headers.
