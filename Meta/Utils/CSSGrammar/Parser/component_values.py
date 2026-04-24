from dataclasses import dataclass
from typing import Union


@dataclass(frozen=True)
class Type:
    name: str
    custom_ident_blacklist: list[str] | None

    def dump(self, indent: int) -> str:
        output = f"{'': >{indent}}Type: {self.name}"

        if self.custom_ident_blacklist:
            output += f" ![{', '.join(self.custom_ident_blacklist)}]"

        return output + "\n"


# https://drafts.csswg.org/css-values-4/#component-types
ComponentValue = Union[Type]
