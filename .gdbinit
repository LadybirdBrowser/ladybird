python
from pathlib import Path
import importlib.util
import os
import traceback
import gdb

try:
    ak_path = Path(os.getcwd()) / "Meta" / "Debuggers" / "gdb" / "AK.py"

    gdb.write(f"[ak] loading: {ak_path}\n")

    if not ak_path.exists():
        raise FileNotFoundError(f"{ak_path} does not exist")

    spec = importlib.util.spec_from_file_location("AK", ak_path)

    if spec is None or spec.loader is None:
        raise ImportError("Failed to create module spec")

    ak = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ak)

    gdb.write("[AK] successfully loaded AK.py\n")

except Exception:
    gdb.write("[AK] failed to load AK.py\n")
    gdb.write(traceback.format_exc())
end
