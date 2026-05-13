import json
import sys

from typing import Any
from typing import Optional

_css_dimensions: Optional[dict[str, Any]] = None


def json_is_valid(dimensions_data: dict[str, Any], json_path: str) -> bool:
    is_valid = True
    most_recent_dimension_name = ""
    for dimension_name, units in dimensions_data.items():
        if dimension_name.lower() < most_recent_dimension_name.lower():
            print(
                f"{json_path}: Dimension `{dimension_name}` is in the wrong position. "
                "Please keep this list alphabetical!",
                file=sys.stderr,
            )
            is_valid = False
        most_recent_dimension_name = dimension_name

        if not isinstance(units, dict):
            print(f"{json_path}: Dimension `{dimension_name}` is not an object", file=sys.stderr)
            is_valid = False
            continue

        most_recent_unit_name = ""
        canonical_unit = None
        for unit_name, unit in units.items():
            # Units should be in alphabetical order
            if unit_name.lower() < most_recent_unit_name.lower():
                print(
                    f"{json_path}: {dimension_name} unit `{unit_name}` is in the wrong position. "
                    "Please keep this list alphabetical!",
                    file=sys.stderr,
                )
                is_valid = False
            most_recent_unit_name = unit_name

            if not isinstance(unit, dict):
                print(f"{json_path}: {dimension_name} unit `{unit_name}` is not an object", file=sys.stderr)
                is_valid = False
                continue

            is_canonical_unit = unit.get("is-canonical-unit") is True
            number_of_canonical_unit = unit.get("number-of-canonical-unit")
            relative_to = unit.get("relative-to")
            provided_count = (
                (1 if is_canonical_unit else 0)
                + (1 if number_of_canonical_unit is not None else 0)
                + (1 if relative_to is not None else 0)
            )
            if provided_count != 1:
                print(
                    f"{json_path}: {dimension_name} unit `{unit_name}` must have exactly 1 of "
                    "`is-canonical-unit: true`, `number-of-canonical-unit`, or `relative-to` provided.",
                    file=sys.stderr,
                )
                is_valid = False
            if is_canonical_unit:
                if canonical_unit is not None:
                    print(
                        f"{json_path}: {dimension_name} unit `{unit_name}` marked canonical, "
                        f"but `{canonical_unit}` was already. Must have exactly 1.",
                        file=sys.stderr,
                    )
                    is_valid = False
                else:
                    canonical_unit = unit_name
            if relative_to is not None:
                if dimension_name == "length":
                    if relative_to not in ("font", "viewport"):
                        print(
                            f"{json_path}: {dimension_name} unit `{unit_name}` is marked as relative to "
                            f"`{relative_to}`, which is unsupported.",
                            file=sys.stderr,
                        )
                        is_valid = False
                else:
                    print(
                        f"{json_path}: {dimension_name} unit `{unit_name}` is marked as relative, "
                        "but only relative length units are currently supported.",
                        file=sys.stderr,
                    )
                    is_valid = False

        if canonical_unit is None:
            print(
                f"{json_path}: {dimension_name} has no unit marked as canonical. Must have exactly 1.",
                file=sys.stderr,
            )
            is_valid = False

    return is_valid


def load_css_dimensions(json_path: str) -> None:
    global _css_dimensions

    with open(json_path, "r", encoding="utf-8") as json_file:
        dimensions_data = json.load(json_file)

    if not isinstance(dimensions_data, dict):
        raise RuntimeError(f"{json_path}: expected a JSON object")

    if not json_is_valid(dimensions_data, json_path):
        raise RuntimeError(f"{json_path}: invalid CSS dimensions data")

    _css_dimensions = dimensions_data


def get_css_dimensions() -> dict[str, Any]:
    if _css_dimensions is None:
        raise RuntimeError("CSS dimensions have not been initialized")

    return _css_dimensions
