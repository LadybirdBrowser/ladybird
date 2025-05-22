#!/usr/bin/env python3
"""
This script will create a new test file and expectations in the Tests/LibWeb directory
"""

import argparse
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent


def create_test(test_name: str, test_type: str, is_async: bool = False) -> None:
    """
    Create a test of a given type with the given test name

    Args:
        test_name (str): Name of the test.
        test_type (str): Type of the test. Currently supports
            "Text", "Layout", "Screenshot" and "Ref""
        is_async (bool, optional): Whether it is an async test. Defaults to False.
    """

    input_prefix = TEST_DIR / test_type / "input" / test_name
    input_file = input_prefix.with_suffix(".html")
    input_dir = input_prefix.parent

    output_prefix = TEST_DIR / test_type / "expected" / test_name
    if test_type in ["Layout", "Text"]:
        output_file = output_prefix.with_suffix(".txt")
    else:
        output_file = output_prefix.with_name(Path(test_name).stem + "-ref.html")
    output_dir = output_prefix.parent

    # Create directories if they don't exist
    input_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    num_sub_levels = len(Path(test_name).parents) - 1
    path_to_include_js = "../" * num_sub_levels + "include.js"

    def generate_boilerplate():
        """
        return a tuple of strings with the input and
        output boilerplate for a given test type
        """

        generic_boilerplate = R"""<!DOCTYPE html>
<head>
<style>
</style>
</head>
<body>
</body>
"""

        if test_type == "Text":
            input_boilerplate = Rf"""<!DOCTYPE html>
<script src="{path_to_include_js}"></script>
<script>
    {f"asyncTest(async (done)" if is_async else "test(()"} => {{
        println("Expected println() output");
    {f"    done();" if is_async else ""}
    }});
</script>
"""
            expected_boilerplate = "Expected println() output\n"

        elif test_type == "Ref":
            input_boilerplate = Rf"""<!DOCTYPE html>
<head>
     <link rel="match" href="{"../" * num_sub_levels}../expected/{Path(test_name).with_suffix("")}-ref.html" />
<style>
</style>
</head>
<body>
</body>
"""

            expected_boilerplate = f"Put equivalently rendering HTML for {test_name} here."

        elif test_type == "Screenshot":
            input_boilerplate = generic_boilerplate
            expected_boilerplate = f"Put equivalently rendering HTML for {test_name} here."

        # layout tests are async agnostic
        elif test_type == "Layout":
            input_boilerplate = generic_boilerplate
            expected_boilerplate = f"""run
 ./Meta/ladybird.sh run headless-browser --run-tests
 "${{LADYBIRD_SOURCE_DIR}}/Tests/LibWeb" --rebaseline -f {input_file}
to produce the expected output for this test
"""
            print("Delete <!DOCTYPE html> and replace it with <!--Quirks mode--> if test should run in quirks mode")

        else:
            # should be unreachable
            raise ValueError(f"UNREACHABLE Invalid test type: {test_type}")

        return (input_boilerplate, expected_boilerplate)

    # Create input and expected files
    input_boilerplate, expected_boilerplate = generate_boilerplate()
    input_file.write_text(input_boilerplate)
    output_file.write_text(expected_boilerplate)

    print(f"{test_type} test '{Path(test_name).with_suffix('.html')}' created successfully.")


def main():
    supported_test_types = ["Ref", "Text", "Screenshot", "Layout"]

    parser = argparse.ArgumentParser(description="Create a new LibWeb Text test file.")
    parser.add_argument("test_name", type=str, help="Name of the test")
    parser.add_argument("test_type", type=str, help="Type of the test", choices=supported_test_types)
    parser.add_argument("--async", action="store_true", help="Flag to indicate if it's an async test", dest="is_async")
    args = parser.parse_args()

    create_test(args.test_name, args.test_type, args.is_async)


if __name__ == "__main__":
    main()
