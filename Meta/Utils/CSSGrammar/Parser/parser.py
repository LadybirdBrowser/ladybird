from Utils.CSSGrammar.Parser.component_values import Keyword
from Utils.CSSGrammar.Parser.grammar_node import CombinatorGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import CombinatorType
from Utils.CSSGrammar.Parser.grammar_node import ComponentValueGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GroupGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import MultiplierGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import OptionalGrammarNode
from Utils.CSSGrammar.Parser.token import Token
from Utils.CSSGrammar.Parser.token import TokenType
from Utils.CSSGrammar.Parser.tokenizer import Tokenizer


class Parser:
    def __init__(self, tokens: list[Token]) -> None:
        self.tokens = tokens
        self.index = 0

    @classmethod
    def parse_value_definition_grammar(cls, input: str) -> GrammarNode:
        parser = cls(Tokenizer.tokenize(input))

        value = parser.parse_alternatives()

        if not parser.peek().is_token_type(TokenType.END_OF_FILE):
            raise SyntaxError("CSSGrammar::Parser: Unexpected trailing input")

        return value

    def parse_alternatives(self) -> GrammarNode:
        children = [self.parse_juxtaposition()]

        while self.peek().is_token_type(TokenType.SINGLE_BAR):
            self.consume()
            children.append(self.parse_juxtaposition())

        if len(children) == 1:
            return children[0]

        return CombinatorGrammarNode(CombinatorType.ALTERNATIVES, children)

    def parse_juxtaposition(self) -> GrammarNode:
        children = [self.parse_component_value()]

        while self.next_token_starts_component_value():
            children.append(self.parse_component_value())

        if len(children) == 1:
            return children[0]

        return CombinatorGrammarNode(CombinatorType.JUXTAPOSITION, children)

    def parse_component_value(self) -> GrammarNode:
        # https://drafts.csswg.org/css-values-4/#component-multipliers
        # FIXME: Support component multipliers

        peeked = self.peek()
        component_value = None

        if peeked.is_token_type(TokenType.OPEN_SQUARE_BRACKET):
            self.consume()
            group = self.parse_alternatives()

            if not self.peek().is_token_type(TokenType.CLOSE_SQUARE_BRACKET):
                raise SyntaxError("CSSGrammar::Parser: Expected ']'")

            # FIXME: Support required groups (e.g. [ <foo>? ]!)
            self.consume()
            component_value = GroupGrammarNode(group)

        if peeked.is_token_type(TokenType.COMPONENT_VALUE):
            component_value = ComponentValueGrammarNode(self.consume().component_value())

        if component_value is None:
            raise SyntaxError("CSSGrammar::Parser: Expected a component value")

        if self.peek().is_token_type(TokenType.QUESTION_MARK):
            self.consume()
            component_value = OptionalGrammarNode(component_value)

        # A single number in curly braces ({A}) indicates that the preceding type, word, or group occurs A times.
        # A comma-separated pair of numbers in curly braces ({A,B}) indicates that the preceding type, word, or group
        # occurs at least A and at most B times. The B may be omitted ({A,}) to indicate that there must be
        # at least A repetitions, with no upper bound on the number of repetitions.
        if self.peek().is_token_type(TokenType.OPEN_CURLY_BRACKET):
            self.consume()

            if not self.peek().is_token_type(TokenType.COMPONENT_VALUE):
                raise SyntaxError("CSSGrammar::Parser: Expected an integer in multiplier")
            min_token = self.consume().component_value()
            assert isinstance(min_token, Keyword)
            minimum = int(min_token.value)

            if self.peek().is_token_type(TokenType.COMMA):
                self.consume()

                if self.peek().is_token_type(TokenType.CLOSE_CURLY_BRACKET):
                    # {A,} — unbounded
                    self.consume()
                    component_value = MultiplierGrammarNode(component_value, minimum, None)
                elif self.peek().is_token_type(TokenType.COMPONENT_VALUE):
                    # {A,B}
                    max_token = self.consume().component_value()
                    assert isinstance(max_token, Keyword)
                    maximum = int(max_token.value)
                    if not self.peek().is_token_type(TokenType.CLOSE_CURLY_BRACKET):
                        raise SyntaxError("CSSGrammar::Parser: Expected '}' to close multiplier")
                    self.consume()
                    component_value = MultiplierGrammarNode(component_value, minimum, maximum)
                else:
                    raise SyntaxError("CSSGrammar::Parser: Expected integer or '}' in multiplier")
            elif self.peek().is_token_type(TokenType.CLOSE_CURLY_BRACKET):
                # {A} — exact count
                self.consume()
                component_value = MultiplierGrammarNode(component_value, minimum, minimum)
            else:
                raise SyntaxError("CSSGrammar::Parser: Expected ',' or '}' after integer in multiplier")

        return component_value

    def next_token_starts_component_value(self) -> bool:
        return self.peek().token_type in (
            TokenType.OPEN_SQUARE_BRACKET,
            TokenType.COMPONENT_VALUE,
        )

    def peek(self, offset: int = 0) -> Token:
        index = min(self.index + offset, len(self.tokens) - 1)
        return self.tokens[index]

    def consume(self) -> Token:
        token = self.peek()

        if not token.is_token_type(TokenType.END_OF_FILE):
            self.index += 1

        return token


def parse_value_definition_grammar(input: str) -> GrammarNode:
    return Parser.parse_value_definition_grammar(input)
