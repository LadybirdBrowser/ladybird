# Helix Configuration
Helix comes with support for `clangd` and `clang-format` out of the box! However, a small bit of configuration is needed for it to work correctly with Ladybird.

The following `.clangd` should be placed in the project root:
```yaml
CompileFlags:
  CompilationDatabase: Build/lagom

Diagnostics:
  UnusedIncludes: None
  MissingIncludes: None
```

You also need to configure the clangd server to not insert headers improperly. To do this, create a `.helix/languages.toml` file in the project root:
```toml
[language-server.ladybird]
command = "clangd"
args = ["--header-insertion=never"]

[[language]]
name = "cpp"
language-servers = ["ladybird"]
```

> Make sure to replace `/path/to/ladybird` with the actual path in the snippet above!
