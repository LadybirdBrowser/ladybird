# Helix Configuration
Helix comes with support for `clangd` and `clang-format` out of the box! However, you also need to configure the clangd server to not insert headers improperly. To do this, create a `.helix/languages.toml` file in the project root:
```toml
[language-server.ladybird]
command = "clangd"
args = ["--header-insertion=never"]

[[language]]
name = "cpp"
language-servers = ["ladybird"]
```
