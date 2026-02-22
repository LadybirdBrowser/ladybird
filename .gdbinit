# Ladybird GDB configuration
# This file is automatically loaded when GDB is started from the project root.
#
# Note: You may need to add the following to your ~/.gdbinit to allow loading project-specific .gdbinit files:
#   add-auto-load-safe-path /path/to/ladybird/.gdbinit
#
# Or to allow all project .gdbinit files:
#   set auto-load safe-path /

python
import sys
from pathlib import Path

# Find the Meta/gdb directory
gdb_scripts_dir = Path(__file__).parent / "Meta" / "gdb" if "__file__" in dir() else None
if gdb_scripts_dir is None:
    import os
    gdb_scripts_dir = Path(os.getcwd()) / "Meta" / "gdb"

init_script = gdb_scripts_dir / "init.py"
if init_script.exists():
    sys.path.insert(0, str(gdb_scripts_dir))
    import init
else:
    print(f"Warning: Could not find GDB init script at {init_script}")
end
