# Visual Studio Code Project Configuration

Visual Studio Code requires some configuration files, and a tailored ``settings.json`` file to understand ladybird.

The WSL Remote extension allows you to use VS Code in Windows while using the normal WSL workflow. This works well, but for code comprehension speed you should put the ladybird directory on your WSL root partition.

The recommended extensions for VS Code include:

- [CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools)
- [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
- [GitLens](https://marketplace.visualstudio.com/items?itemName=eamodio.gitlens)

## Configuration

Run `./Meta/ladybird.sh build` at least once to kick off downloading and building vcpkg dependencies.

The CMake Tools plugin should automatically detect the `CMakePresets.json` at the root of the repository.
Selecting and activating the `default` preset should be enough to get started after the initial build.
You can also use the `Debug` preset to build with debug symbols, or the `Sanitizer` preset to build with ASAN/UBSAN.

For additional settings recommendations, see the [Settings](#settings) section below.

## Code comprehension

Clangd has the best support for modern compilers, especially if configured as noted below. The Microsoft C/C++ tools can work, but may require more configuration.

### clangd

The official clangd extension can be used for C++ comprehension. It is recommended in general, as it is most likely to work on all platforms.

clangd uses ``compile_commands.json`` files to understand the project. CMake will generate these in Build/release.

Run ``./Meta/ladybird.sh run ladybird`` at least once to generate the ``compile_commands.json`` file.

#### Known issues

- clangd has a tendency to crash when stressing bleeding edge compiler features. You can usually just restart it via the command palette. If that doesn't help, close currently open C++ files and/or switch branches before restarting, which helps sometimes.

### DSL syntax highlighting

There's a syntax highlighter extension for domain specific language files (.idl, .ipc) called "SerenityOS DSL Syntax Highlight", available [here](https://marketplace.visualstudio.com/items?itemName=kleinesfilmroellchen.serenity-dsl-syntaxhighlight) or [here](https://open-vsx.org/extension/kleinesfilmroellchen/serenity-dsl-syntaxhighlight).
The extension provides syntax highlighting for these files, [Web IDL](https://webidl.spec.whatwg.org/), and LibJS's
serialization format (no extension) as output by js with the -d option.

### Microsoft C/C++ tools

Note that enabling the extension in the same workspace as the  clangd and clang-format extensions will cause conflicts.
If you choose to use Microsoft C/C++ Tools rather than clangd and clang-format, use the
following ``c_cpp_properties.json`` to circumvent some errors. Even with the configuration in place, the extension will likely still report errors related to types and methods not being found.

<details>
<summary>.vscode/c_cpp_properties.json</summary>

```json
{
    "configurations": [
        {
            "name": "ladybird-gcc",
            "includePath": [
                "${workspaceFolder}",
                "${workspaceFolder}/Build/release/",
                "${workspaceFolder}/Build/release/Libraries",
                "${workspaceFolder}/Build/release/Services",
                "${workspaceFolder}/Libraries",
                "${workspaceFolder}/Services"
            ],
            "defines": [
                "DEBUG"
            ],
            "cStandard": "c17",
            "cppStandard": "c++23",
            "intelliSenseMode": "linux-gcc-x86",
            "compileCommands": "Build/release/compile_commands.json",
            "compilerArgs": [
                "-Wall",
                "-Wextra",
                "-Werror"
            ],
            "browse": {
                "path": [
                    "${workspaceFolder}",
                    "${workspaceFolder}/Build/release/",
                    "${workspaceFolder}/Build/release/Libraries",
                    "${workspaceFolder}/Build/release/Services",
                    "${workspaceFolder}/Libraries",
                    "${workspaceFolder}/Services"
                ],
                "limitSymbolsToIncludedHeaders": true,
                "databaseFilename": "${workspaceFolder}/Build/release/"
            }
        }
    ],
    "version": 4
}
```
</details>

## Formatting

clangd provides code formatting out of the box using the ``clang-format`` engine. ``clang-format`` support is also included with the Microsoft C/C++ tools (see above). The settings below include a key that makes the Microsoft extension use the proper style.

## Settings

These belong in the `.vscode/settings.json` of Ladybird.

```json
{
    // Excluding the generated directories keeps your file view clean and speeds up search.
    "files.exclude": {
        "**/.git": true,
        "Toolchain/Local/**": true,
        "Toolchain/Tarballs/**": true,
        "Toolchain/Build/**": true,
        "Build/**": true,
    },
    "search.exclude": {
        "**/.git": true,
        "Toolchain/Local/**": true,
        "Toolchain/Tarballs/**": true,
        "Toolchain/Build/**": true,
        "Build/**": true,
    },
    // Force clang-format to respect Ladybird's .clang-format style file. This is not necessary if you're not using the Microsoft C++ extension.
    "C_Cpp.clang_format_style": "file",
    // Tab settings
    "editor.tabSize": 4,
    "editor.useTabStops": false,
    // format trailing new lines
    "files.trimFinalNewlines": true,
    "files.insertFinalNewline": true,
    // git commit message length
    "git.inputValidationLength": 72,
    "git.inputValidationSubjectLength": 72,
    // If clangd was obtained from a package manager, its path can be set here.
    "clangd.path": "clangd-18",
    "clangd.arguments": [
        "--header-insertion=never" // See https://github.com/clangd/clangd/issues/1247
    ]
}
```

## Customization

### Custom Tasks

You can create custom tasks (`.vscode/tasks.json`) to quickly compile Ladybird.
The following three example tasks should suffice in most situations, and allow you to specify the build system to use, as well as give you error highlighting.

<details>
<summary>.vscode/tasks.json</summary>

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "build lagom",
            "type": "shell",
            "problemMatcher": [
                {
                    "base": "$gcc",
                    "fileLocation": [
                        "relative",
                        "${workspaceFolder}/Build/release"
                    ]
                }
            ],
            "command": [
                "bash"
            ],
            "args": [
                "-c",
                "\"Meta/ladybird.sh build\""
            ],
            "presentation": {
                "echo": true,
                "reveal": "always",
                "focus": false,
                "group": "build",
                "panel": "shared",
                "showReuseMessage": true,
                "clear": true
            }
        },
        {
            "label": "build",
            "type": "shell",
            "command": "bash",
            "args": [
                "-c",
                "Meta/ladybird.sh build"
            ],
            "problemMatcher": [
                {
                    "base": "$gcc",
                    "fileLocation": [
                        "relative",
                        "${workspaceFolder}/Build/release"
                    ]
                },
                {
                    "source": "gcc",
                    "fileLocation": [
                        "relative",
                        "${workspaceFolder}/Build/release"
                    ],
                    "pattern": [
                        {
                            "regexp": "^([^\\s]*\\.S):(\\d*): (.*)$",
                            "file": 1,
                            "location": 2,
                            "message": 3
                        }
                    ]
                }
            ],
            "group": {
                "kind": "build",
                "isDefault": true
            }
        },
        {
            "label": "launch",
            "type": "shell",
            "command": "bash",
            "args": [
                "-c",
                "Meta/ladybird.sh run ladybird"
            ],
            "options": {
                "env": {
                    // Put your custom run configuration here
                }
            },
            "problemMatcher": [
                {
                    "base": "$gcc",
                    "fileLocation": [
                        "relative",
                        "${workspaceFolder}/Build/release"
                    ]
                },
                {
                    "source": "gcc",
                    "fileLocation": [
                        "relative",
                        "${workspaceFolder}/Build/release"
                    ],
                    "pattern": [
                        {
                            "regexp": "^([^\\s]*\\.S):(\\d*): (.*)$",
                            "file": 1,
                            "location": 2,
                            "message": 3
                        }
                    ]
                },
                {
                    "source": "Assertion Failed",
                    "owner": "cpp",
                    "pattern": [
                        {
                            "regexp": "ASSERTION FAILED: (.*)$",
                            "message": 1
                        },
                        {
                            "regexp": "^((?:.*)\\.(h|cpp|c|S)):(\\d*)$",
                            "file": 1,
                            "location": 3
                        }
                    ],
                    "fileLocation": [
                        "relative",
                        "${workspaceFolder}/Build/release"
                    ]
                }
            ]
        }
    ]
}
```

</details>

### Debugging
#### Mac
If you want to run the debugger, first place the content below in `.vscode/launch.json` in the root of the project.

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Attach to WebContent",
            "type": "lldb",
            "request": "attach",
            "program": "${workspaceFolder}/Build/ladybird-debug/bin/Ladybird.app/Contents/MacOS/WebContent",
        }
    ],
}
```

then run Ladybird with the debug preset and with the `--debug-process WebContent` flag:

```bash
CC=$(brew --prefix llvm)/bin/clang CXX=$(brew --prefix llvm)/bin/clang++ BUILD_PRESET=Debug ./Meta/ladybird.sh run ladybird --debug-process WebContent
```

Running Ladybird in this way will pause execution until a debugger is attached. You can then run the debugger by going to the **Run and Debug** menu and selecting the **Attach to WebContent** configuration.

#### Linux
For Linux, the `launch.json` will instead be the file below.

```json
{
  "version": "2.0.0",
  "configurations": [
    {
      "name": "Attach and debug",
      "type": "cppdbg",
      "request": "attach",
      "program": "${workspaceRoot}/Build/ladybird-debug/libexec/WebContent",
      "MIMode": "gdb",
    },
  ]
}
```

Running Ladybird as follows:

```bash
BUILD_PRESET=Debug Meta/ladybird.sh run ladybird --debug-process WebContent
```

Then follow the same steps found in the Mac section.

### License snippet

The following snippet may be useful if you want to quickly generate a license header, put it in `.vscode/ladybird.code-snippets`:
```json
{
    "License": {
        "scope": "cpp,c",
        "prefix": "license",
        "body": [
            "/*",
            " * Copyright (c) $CURRENT_YEAR, ${1:Your Name} <${2:YourName@Email.com}>.",
            " *",
            " * SPDX-License-Identifier: BSD-2-Clause",
            " */"
        ],
        "description": "License header"
    }
}
```
