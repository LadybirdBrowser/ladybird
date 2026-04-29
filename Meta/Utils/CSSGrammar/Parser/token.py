from dataclasses import dataclass
from enum import Enum
from typing import Optional

from Utils.CSSGrammar.Parser.component_values import ComponentValue


class TokenType(Enum):
    END_OF_FILE = "end-of-file"
    SINGLE_BAR = "single-bar"
    COMPONENT_VALUE = "component-value"


@dataclass(frozen=True)
class Token:
    token_type: TokenType
    value: Optional[ComponentValue]

    @classmethod
    def create(cls, token_type: TokenType) -> "Token":
        return cls(token_type, None)

    @classmethod
    def create_component_value(cls, component_value: ComponentValue) -> "Token":
        return cls(TokenType.COMPONENT_VALUE, component_value)

    def is_token_type(self, token_type: TokenType) -> bool:
        return self.token_type == token_type

    def component_value(self) -> ComponentValue:
        assert self.token_type == TokenType.COMPONENT_VALUE and self.value is not None

        return self.value
