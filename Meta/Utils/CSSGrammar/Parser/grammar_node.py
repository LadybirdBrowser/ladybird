from dataclasses import dataclass
from enum import Enum
from typing import Optional

from Utils.CSSGrammar.Parser.component_values import ComponentValue


class GrammarNode:
    def dump(self, indent: int = 0) -> str:
        raise NotImplementedError


# https://drafts.csswg.org/css-values-4/#component-types
@dataclass(frozen=True)
class ComponentValueGrammarNode(GrammarNode):
    component_value: ComponentValue

    def dump(self, indent: int = 0) -> str:
        return f"{'': >{indent}}ComponentValue\n" + self.component_value.dump(indent + 2)


class CombinatorType(Enum):
    # https://drafts.csswg.org/css-values-4/#component-combinators
    # Juxtaposing components means that all of them must occur, in the given order.
    JUXTAPOSITION = "Juxtaposition"

    # https://drafts.csswg.org/css-values-4/#comb-one
    # A bar (|) separates two or more alternatives: exactly one of them must occur.
    ALTERNATIVES = "Alternatives"


# https://drafts.csswg.org/css-values-4/#component-combinators
@dataclass(frozen=True)
class CombinatorGrammarNode(GrammarNode):
    combinator_type: CombinatorType
    children: list[GrammarNode]

    def dump(self, indent: int = 0) -> str:
        output = f"{'': >{indent}}Combinator({self.combinator_type.value}):\n"

        for child in self.children:
            output += child.dump(indent + 2)

        return output


@dataclass(frozen=True)
class GroupGrammarNode(GrammarNode):
    child: GrammarNode

    def dump(self, indent: int = 0) -> str:
        return f"{'': >{indent}}Group:\n" + self.child.dump(indent + 2)


@dataclass(frozen=True)
class OptionalGrammarNode(GrammarNode):
    child: GrammarNode

    def dump(self, indent: int = 0) -> str:
        return f"{'': >{indent}}Optional:\n" + self.child.dump(indent + 2)


@dataclass(frozen=True)
class MultiplierGrammarNode(GrammarNode):
    child: GrammarNode
    minimum: int
    maximum: Optional[int]

    def dump(self, indent: int = 0) -> str:
        if self.maximum is None:
            return f"{'': >{indent}}Multiplier {{{self.minimum},}}:\n" + self.child.dump(indent + 2)
        if self.minimum == self.maximum:
            return f"{'': >{indent}}Multiplier {{{self.minimum}}}:\n" + self.child.dump(indent + 2)
        return f"{'': >{indent}}Multiplier {{{self.minimum},{self.maximum}}}:\n" + self.child.dump(indent + 2)
