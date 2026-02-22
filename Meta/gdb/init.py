"""
GDB initialization script for Ladybird.
"""

import importlib.util

from pathlib import Path


def load_pretty_printers():
    """Load all Python scripts from the PrettyPrinters directory."""
    printers_dir = Path(__file__).parent / "PrettyPrinters"

    if not printers_dir.exists():
        print(f"Warning: PrettyPrinters directory not found at {printers_dir}")
        return

    for script in sorted(printers_dir.glob("*.py")):
        if script.name.startswith("_"):
            continue

        try:
            spec = importlib.util.spec_from_file_location(script.stem, script)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            print(f"Loaded {script.name}")
        except Exception as e:
            print(f"Error loading {script.name}: {e}")


load_pretty_printers()
