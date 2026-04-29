from dataclasses import dataclass
from math import inf
from typing import Optional
from typing import Union


@dataclass(frozen=True)
class NumericTypeRangeRestriction:
    minimum: float
    maximum: float

    def dump(self) -> str:
        return f"[{bound_value_to_string(self.minimum)},{bound_value_to_string(self.maximum)}]"


def bound_value_to_string(value: float) -> str:
    if value == -inf:
        return "-∞"
    if value == inf:
        return "∞"
    return str(value)


def is_dimension_type(type_name: str) -> bool:
    # NB: Keep this up to date with the list of dimensions in Units.json
    return type_name in (
        "angle",
        "decibel",
        "flex",
        "frequency",
        "length",
        "resolution",
        "time",
    )


def is_dimension_percentage_mix_type(type_name: str) -> bool:
    # https://drafts.csswg.org/css-values-4/#mixed-percentages
    return type_name in (
        "angle-percentage",
        "frequency-percentage",
        "length-percentage",
        "time-percentage",
    )


def is_numeric_type(type_name: str) -> bool:
    # https://drafts.csswg.org/css-values-4/#numeric-data-types
    return type_name in ("integer", "number", "percentage") or is_dimension_type(type_name)


@dataclass(frozen=True)
class Type:
    name: str
    custom_ident_blacklist: list[str]
    numeric_type_accepted_range: Optional[NumericTypeRangeRestriction]

    def dump(self, indent: int) -> str:
        output = f"{'': >{indent}}Type: {self.name}"

        if self.custom_ident_blacklist:
            output += f" ![{', '.join(self.custom_ident_blacklist)}]"

        if self.numeric_type_accepted_range:
            output += f" {self.numeric_type_accepted_range.dump()}"

        return output + "\n"


@dataclass(frozen=True)
class Keyword:
    value: str

    def dump(self, indent: int) -> str:
        return f"{'': >{indent}}Keyword: {self.value}\n"


# https://drafts.csswg.org/css-values-4/#component-types
ComponentValue = Union[Type, Keyword]
