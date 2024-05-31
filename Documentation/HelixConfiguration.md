# Helix Configuration
Helix comes with support for `clangd` and `clang-format` out of the box! However, a small bit of configuration is needed for it to work correctly with Ladybird.

The following `.clangd` should be placed in the project root:
```yaml
CompileFlags:
  CompilationDatabase: Build/x86_64 # Or whatever architecture you're targeting, e.g. aarch64
  Add: [-D__serenity__]

Diagnostics:
  UnusedIncludes: None
  MissingIncludes: None
```

You also need to configure the clangd server to detect headers properly from the Serenity toolchain. To do this, create a `.helix/languages.toml` file in the project root:
```toml
[language-server.serenity]
command = "clangd"
args = ["--query-driver=/path/to/serenity/Toolchain/Local/**/*", "--header-insertion=never"]

[[language]]
name = "cpp"
language-servers = ["serenity"]
```

> Make sure to replace `/path/to/serenity` with the actual path in the snippet above!
