from dataclasses import dataclass
from typing import Union


@dataclass(frozen=True)
class Type:
    name: str

    def dump(self, indent: int) -> str:
        return f"{'': >{indent}}Type: {self.name}\n"


# https://drafts.csswg.org/css-values-4/#component-types
ComponentValue = Union[Type]
