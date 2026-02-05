# Disable flake linting for this file since it flags "config" as a non-existent variable
# flake8: noqa

import os
import lit.formats
import lit.util
from lit.llvm import llvm_config
from lit.llvm.subst import ToolSubst
from lit.llvm.subst import FindTool

config.name = "ClangPlugins"
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)
config.suffixes = [".cpp"]
config.test_source_root = os.path.dirname(__file__)
llvm_config.use_default_substitutions()
llvm_config.use_clang()
config.substitutions.append(("%target_triple", config.target_triple))
config.substitutions.append(("%PATH%", config.environment["PATH"]))

plugin_includes = " ".join(f"-I{s}" for s in config.plugin_includes.split(";"))
plugin_opts = " ".join(s for s in config.plugin_opts.split(";"))
config.substitutions.append(("%plugin_opts%", f"{plugin_opts} {plugin_includes}"))

tools = ["clang", "clang++"]
llvm_config.add_tool_substitutions(tools, config.llvm_tools_dir)
