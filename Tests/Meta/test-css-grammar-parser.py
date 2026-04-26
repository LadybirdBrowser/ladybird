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

    def test_parse_unitless_numeric_type_bracketed_range_notation(self) -> None:
        for value, expected in (
            ("<number [1,∞]>", "number [1.0,∞]"),
            ("<integer [1,∞]>", "integer [1.0,∞]"),
            ("<percentage [1,100]>", "percentage [1.0,100.0]"),
        ):
            with self.subTest(value=value):
                syntax = parse_value_definition_grammar(value)
                self.assertEqual(
                    syntax.dump(),
                    f"""ComponentValue
  Type: {expected}
""",
                )

    def test_parse_type_percentage_bracketed_range_notation(self) -> None:
        syntax = parse_value_definition_grammar("<length-percentage [0,∞]>")
        self.assertEqual(
            syntax.dump(),
            """ComponentValue
  Type: length-percentage [0.0,∞]
""",
        )

    def test_parse_numeric_type_without_bracketed_range_notation_defaults_to_infinite_range(self) -> None:
        syntax = parse_value_definition_grammar("<number>")
        self.assertEqual(
            syntax.dump(),
            """ComponentValue
  Type: number [-∞,∞]
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
    Type: length [-∞,∞]
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

    def test_reject_invalid_bracketed_range_notation(self) -> None:
        for value in (
            "<number [0 ∞]>",
            "<number [0,∞>",
            "<number [,∞]>",
            "<number [1,]>",
            "<number [1]>",
        ):
            with self.subTest(value=value):
                with self.assertRaises(SyntaxError):
                    parse_value_definition_grammar(value)

    def test_reject_bracketed_range_notation_on_non_numeric_data_types(self) -> None:
        for value in (
            "<color [0,∞]>",
            "<custom-ident [0,∞]>",
        ):
            with self.subTest(value=value):
                with self.assertRaises(SyntaxError):
                    parse_value_definition_grammar(value)

    def test_reject_units_on_unitless_bracketed_range_bounds(self) -> None:
        for value in (
            "<number [1px,∞]>",
            "<integer [1px,∞]>",
            "<percentage [1px,100px]>",
        ):
            with self.subTest(value=value):
                with self.assertRaises(SyntaxError):
                    parse_value_definition_grammar(value)

    def test_reject_units_on_infinite_bracketed_range_bounds(self) -> None:
        for value in (
            "<length [0,∞px]>",
            "<length [-∞px,0]>",
        ):
            with self.subTest(value=value):
                with self.assertRaises(SyntaxError):
                    parse_value_definition_grammar(value)

    def test_reject_non_zero_non_infinite_bounds_on_types_with_units_not_resolvable_at_parse_time(self) -> None:
        for value in (
            "<length [1px,10px]>",
            "<angle-percentage [1deg,360deg]>",
        ):
            with self.subTest(value=value):
                with self.assertRaises(SyntaxError):
                    parse_value_definition_grammar(value)

    def test_unit_required_for_non_zero_non_infinite_bounds(self) -> None:
        with self.assertRaises(SyntaxError):
            parse_value_definition_grammar("<angle [-90,90]>")


if __name__ == "__main__":
    unittest.main()
