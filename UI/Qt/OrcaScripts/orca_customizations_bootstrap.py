# Ladybird: register custom app-specific Orca script for Orca 49.7+.
#
# Orca 49.7 removed support for the ~/.local/share/orca/orca-scripts/ directory as a means to directly load custom user/
# app-supplied scripts. But we still want to put our custom script there — so we use this bootstrap script to re-enable
# loading from that directory. We plumb the necessary handling into Orca's supported orca-customizations.py file — but
# mark it with delimiters. That ensures we don’t change any content outside the delimiters (the user’s own additions).

import importlib as _ladybird_importlib
import os as _ladybird_os
import sys as _ladybird_sys

_ladybird_scripts_dir = _ladybird_os.path.join(
    _ladybird_os.path.dirname(_ladybird_os.path.abspath(__file__)),
    "orca-scripts",
)
if _ladybird_scripts_dir not in _ladybird_sys.path:
    _ladybird_sys.path.insert(0, _ladybird_scripts_dir)

try:
    from orca import script_manager as _ladybird_script_manager

    _ladybird_original_new_named_script = _ladybird_script_manager.ScriptManager._new_named_script

    def _ladybird_patched_new_named_script(self, app, name):
        if app and name:
            candidate = _ladybird_os.path.join(_ladybird_scripts_dir, name, "__init__.py")
            if _ladybird_os.path.exists(candidate):
                try:
                    module = _ladybird_importlib.import_module(name)
                    if hasattr(module, "get_script"):
                        return module.get_script(app)
                    if hasattr(module, "Script"):
                        return module.Script(app)
                except Exception:
                    pass
        return _ladybird_original_new_named_script(self, app, name)

    _ladybird_script_manager.ScriptManager._new_named_script = _ladybird_patched_new_named_script
except ImportError:
    pass
