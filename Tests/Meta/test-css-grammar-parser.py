import os
import sys
import unittest

from pathlib import Path

sys.path.insert(0, str(Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Meta"))

from Utils.CSSGrammar.Parser.parser import parse_value_definition_grammar


class TestCSSGrammarParser(unittest.TestCase):
    def test_parse_type_reference(self) -> None:
        syntax = parse_value_definition_grammar("<foo>")
        self.assertEqual(
            syntax.dump(),
            """ComponentValue
  Type: foo
""",
        )

    def test_parse_hyphenated_type_reference(self) -> None:
        syntax = parse_value_definition_grammar("<foo-bar>")
        self.assertEqual(
            syntax.dump(),
            """ComponentValue
  Type: foo-bar
""",
        )

    def test_parse_custom_ident_blacklist(self) -> None:
        syntax = parse_value_definition_grammar("<custom-ident ![foo, bar-baz]>")
        self.assertEqual(
            syntax.dump(),
            """ComponentValue
  Type: custom-ident ![foo, bar-baz]
""",
        )

    def test_parse_keyword(self) -> None:
        syntax = parse_value_definition_grammar("auto")
        self.assertEqual(
            syntax.dump(),
            """ComponentValue
  Keyword: auto
""",
        )

    def test_parse_hyphenated_keyword(self) -> None:
        syntax = parse_value_definition_grammar("max-content")
        self.assertEqual(
            syntax.dump(),
            """ComponentValue
  Keyword: max-content
""",
        )

    def test_parse_keyword_alternatives(self) -> None:
        syntax = parse_value_definition_grammar("auto | none | <length>")
        self.assertEqual(
            syntax.dump(),
            """Combinator(Alternatives):
  ComponentValue
    Keyword: auto
  ComponentValue
    Keyword: none
  ComponentValue
    Type: length
""",
        )

    def test_parse_alternatives(self) -> None:
        syntax = parse_value_definition_grammar("<foo> | <bar> | <baz>")
        self.assertEqual(
            syntax.dump(),
            """Combinator(Alternatives):
  ComponentValue
    Type: foo
  ComponentValue
    Type: bar
  ComponentValue
    Type: baz
""",
        )

    def test_parse_ignores_whitespace_around_tokens(self) -> None:
        syntax = parse_value_definition_grammar("  <foo>\t|\n<bar>  ")
        self.assertEqual(
            syntax.dump(),
            """Combinator(Alternatives):
  ComponentValue
    Type: foo
  ComponentValue
    Type: bar
""",
        )

    def test_reject_empty_input(self) -> None:
        with self.assertRaises(SyntaxError):
            parse_value_definition_grammar("")

    def test_reject_standalone_bar(self) -> None:
        with self.assertRaises(SyntaxError):
            parse_value_definition_grammar("|")

    def test_reject_trailing_bar(self) -> None:
        with self.assertRaises(SyntaxError):
            parse_value_definition_grammar("<foo> |")

    def test_reject_invalid_type_reference(self) -> None:
        for value in (
            "<>",
            "<foo",
            "<foo/auto>",
            "<length ![foo]>",
            "<custom-ident ![]>",
            "<custom-ident ![foo bar]>",
            "<custom-ident ![foo,]>",
            "<custom-ident ![foo>",
        ):
            with self.subTest(value=value):
                with self.assertRaises(SyntaxError):
                    parse_value_definition_grammar(value)


if __name__ == "__main__":
    unittest.main()
