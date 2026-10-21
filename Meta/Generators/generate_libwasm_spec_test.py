import json
import struct
import subprocess
import sys

from dataclasses import dataclass
from pathlib import Path
from typing import Any
from typing import Literal
from typing import Optional
from typing import Union

TEST_MODULES_TO_SKIP: set[str] = set()
TESTS_TO_SKIP: set[str] = set()


with open(Path(__file__).parent / "wasm_unimplemented_tests.txt", "r") as f:
    all_skipped_tests = (x.strip() for x in f.readlines() if not x.startswith("#"))
    for test in all_skipped_tests:
        if test.startswith("module "):
            TEST_MODULES_TO_SKIP.add(test[len("module ") :] + ".wasm")
        elif test.startswith("test "):
            TESTS_TO_SKIP.add(test[len("test ") :])


class ParseException(Exception):
    pass


class GenerateException(Exception):
    pass


@dataclass
class WasmPrimitiveValue:
    kind: Literal["i32", "i64", "f32", "f64", "externref", "funcref"]
    value: Optional[str]


@dataclass
class WasmVector:
    lanes: list[str]
    num_bits: int


@dataclass
class WasmGCValue:
    kind: str
    value: Optional[str] = None


@dataclass
class EitherOf:
    options: list["WasmValue"]


WasmValue = Union[WasmPrimitiveValue, WasmVector, WasmGCValue, EitherOf]


@dataclass
class ModuleCommand:
    line: int
    file_name: Path
    name: Optional[str]


# An anonymous `(module definition ...)`.
@dataclass
class ModuleDefinitionCommand:
    line: int
    file_name: Path


@dataclass
class Invoke:
    field: str
    args: list[WasmValue]
    module: Optional[str]


@dataclass
class Get:
    field: str
    module: Optional[str]


Action = Union[Invoke, Get]


@dataclass
class Register:
    line: int
    name: Optional[str]
    as_: str


@dataclass
class AssertReturn:
    line: int
    action: Action
    expected: Optional[WasmValue]


@dataclass
class AssertTrap:
    line: int
    messsage: str
    action: Action


@dataclass
class AssertException:
    line: int
    action: Action


@dataclass
class ActionCommand:
    line: int
    action: Action


@dataclass
class AssertInvalid:
    line: int
    filename: Path
    message: str


Command = Union[
    ModuleCommand,
    ModuleDefinitionCommand,
    AssertReturn,
    AssertTrap,
    ActionCommand,
    AssertInvalid,
    AssertException,
    Register,
]


@dataclass
class ArithmeticNan:
    num_bits: int


@dataclass
class CanonicalNan:
    num_bits: int


@dataclass
class GeneratedVector:
    repr: str
    num_bits: int


@dataclass
class GeneratedEitherOf:
    options: list["GeneratedValue"]


@dataclass
class GeneratedAnyFuncRef:
    pass


# `(ref.extern)` with no index: any non-null extern reference.
@dataclass
class GeneratedAnyExternRef:
    pass


# A gc reference expectation with no concrete index, e.g. `(ref.struct)`: any non-null
# reference. The harness cannot inspect the heap type, so only non-nullness is checked.
@dataclass
class GeneratedAnyGCRef:
    kind: str


GeneratedValue = Union[
    str,
    ArithmeticNan,
    CanonicalNan,
    GeneratedVector,
    GeneratedEitherOf,
    GeneratedAnyFuncRef,
    GeneratedAnyExternRef,
    GeneratedAnyGCRef,
]


@dataclass
class WastDescription:
    source_filename: str
    commands: list[Command]


@dataclass
class Context:
    current_module_name: str
    has_unclosed: bool


def parse_value(arg: dict[str, Any]) -> WasmValue:
    type_ = arg["type"]
    if type_ in ("i32", "i64", "f32", "f64"):
        return WasmPrimitiveValue(type_, arg["value"])
    if type_ in ("externref", "funcref"):
        return WasmPrimitiveValue(type_, arg["value"] if "value" in arg else None)
    if type_ == "refnull":
        return WasmPrimitiveValue("externref", "null")
    if type_ == "nullfuncref":
        return WasmPrimitiveValue("funcref", "null")
    if type_ == "v128":
        if not isinstance(arg["value"], list):
            raise ParseException("Got unknown type for Wasm value")
        num_bits = int(arg["lane_type"][1:])
        return WasmVector(arg["value"], num_bits)
    if type_ in (
        "arrayref",
        "structref",
        "eqref",
        "anyref",
        "i31ref",
        "exnref",
        "nullref",
        "nullexnref",
        "nullexternref",
    ):
        return WasmGCValue(type_, arg.get("value"))
    if type_ == "either":
        return EitherOf([parse_value(opt) for opt in arg["values"]])
    raise ParseException(f"Unknown value type: {type_}")


def parse_args(raw_args: list[dict[str, str]]) -> list[WasmValue]:
    return [parse_value(arg) for arg in raw_args]


def parse_action(action: dict[str, Any]) -> Action:
    action_type = action["type"]
    if action_type == "invoke":
        return Invoke(action["field"], parse_args(action["args"]), action.get("module"))
    if action_type == "get":
        return Get(action["field"], action.get("module"))
    raise ParseException(f"Action not implemented: {action_type}")


def module_binary_filename(raw_cmd: dict[str, str]) -> Path:
    return Path(raw_cmd["filename"] if raw_cmd.get("module_type") != "text" else raw_cmd["binary_filename"])


def parse(raw: dict[str, Any]) -> WastDescription:
    commands: list[Command] = []
    defined_modules: dict[str, Path] = {}
    for raw_cmd in raw["commands"]:
        line = raw_cmd["line"]
        cmd: Command
        cmd_type = raw_cmd["type"]
        if cmd_type == "module":
            cmd = ModuleCommand(line, module_binary_filename(raw_cmd), raw_cmd.get("name"))
        elif cmd_type == "module_definition":
            if "name" in raw_cmd:
                defined_modules[raw_cmd["name"]] = module_binary_filename(raw_cmd)
                continue
            cmd = ModuleDefinitionCommand(line, module_binary_filename(raw_cmd))
        elif cmd_type == "module_instance":
            cmd = ModuleCommand(line, defined_modules[raw_cmd["module"]], raw_cmd.get("instance"))
        elif cmd_type == "action":
            cmd = ActionCommand(line, parse_action(raw_cmd["action"]))
        elif cmd_type == "register":
            cmd = Register(line, raw_cmd.get("name"), raw_cmd["as"])
        elif cmd_type == "assert_return":
            cmd = AssertReturn(
                line,
                parse_action(raw_cmd["action"]),
                (parse_value(raw_cmd["expected"][0]) if len(raw_cmd["expected"]) == 1 else None),
            )
        elif cmd_type in ("assert_trap", "assert_exhaustion"):
            cmd = AssertTrap(line, raw_cmd["text"], parse_action(raw_cmd["action"]))
        elif cmd_type in ("assert_invalid", "assert_malformed", "assert_uninstantiable", "assert_unlinkable"):
            if raw_cmd.get("module_type") == "text":
                continue
            cmd = AssertInvalid(line, module_binary_filename(raw_cmd), raw_cmd["text"])
        elif cmd_type == "assert_exception":
            cmd = AssertException(line, parse_action(raw_cmd["action"]))
        else:
            raise ParseException(f"Unknown command type: {cmd_type}")
        commands.append(cmd)

    return WastDescription(raw["source_filename"], commands)


def escape(s: str) -> str:
    return s.replace('"', '\\"')


# The legacy exception handling tests write `try` in a folded form, (try bt (do ...) (catch x ...) (delegate l)), that
# wasm-tools does not parse. It does parse the flat form, try bt ... catch x ... end, which is what these turn it into.
# The folded `try`s of the tests are all instructions of their own, never operands, where the flat form is allowed.
# Every token keeps its line, so that the tests keep the line numbers they are named by.
@dataclass
class WastToken:
    text: str
    line: int


@dataclass
class WastList:
    items: list
    open_line: int
    close_line: int


def tokenize_wast(text: str) -> list[WastToken]:
    tokens = []
    position = 0
    line = 1
    while position < len(text):
        character = text[position]
        if character == "\n":
            line += 1
            position += 1
        elif character.isspace():
            position += 1
        elif text.startswith(";;", position):
            end = text.find("\n", position)
            position = len(text) if end == -1 else end
        elif text.startswith("(;", position):
            depth = 0
            while position < len(text):
                if text.startswith("(;", position):
                    depth += 1
                    position += 2
                elif text.startswith(";)", position):
                    depth -= 1
                    position += 2
                    if depth == 0:
                        break
                else:
                    line += text[position] == "\n"
                    position += 1
        elif character in "()":
            tokens.append(WastToken(character, line))
            position += 1
        elif character == '"':
            end = position + 1
            while text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            tokens.append(WastToken(text[position : end + 1], line))
            line += text.count("\n", position, end + 1)
            position = end + 1
        else:
            end = position
            while end < len(text) and not text[end].isspace() and text[end] not in '()";':
                end += 1
            tokens.append(WastToken(text[position:end], line))
            position = end
    return tokens


def parse_wast_tokens(tokens: list[WastToken], position: int = 0) -> tuple[list, int]:
    items: list = []
    while position < len(tokens):
        token = tokens[position]
        if token.text == "(":
            child, position = parse_wast_tokens(tokens, position + 1)
            close_line = tokens[position - 1].line if position - 1 < len(tokens) else token.line
            items.append(WastList(child, token.line, close_line))
        elif token.text == ")":
            return items, position + 1
        else:
            items.append(token)
            position += 1
    return items, position


def head_of(item) -> Optional[str]:
    if isinstance(item, WastList) and item.items and isinstance(item.items[0], WastToken):
        return item.items[0].text
    return None


def is_folded_try(item) -> bool:
    return head_of(item) == "try" and any(head_of(child) == "do" for child in item.items)


def unfold_legacy_try(items: list) -> list:
    unfolded: list = []
    for item in items:
        if isinstance(item, WastToken):
            unfolded.append(item)
            continue
        if not is_folded_try(item):
            unfolded.append(WastList(unfold_legacy_try(item.items), item.open_line, item.close_line))
            continue
        ends_with_delegate = False
        for child in item.items:
            head = head_of(child)
            if head not in ("do", "catch", "catch_all", "delegate"):
                unfolded.extend(unfold_legacy_try([child]))
            elif head == "do":
                unfolded.extend(unfold_legacy_try(child.items[1:]))
            elif head == "catch":
                unfolded.extend(child.items[:2])
                unfolded.extend(unfold_legacy_try(child.items[2:]))
            elif head == "catch_all":
                unfolded.append(child.items[0])
                unfolded.extend(unfold_legacy_try(child.items[1:]))
            else:
                unfolded.extend(child.items)
                ends_with_delegate = True
        if not ends_with_delegate:
            unfolded.append(WastToken("end", item.close_line))
    return unfolded


def serialize_wast(items: list) -> str:
    output: list[str] = []
    line = 1

    def emit(text: str, at_line: int):
        nonlocal line
        if at_line > line:
            output.append("\n" * (at_line - line))
            line = at_line
        output.append(text + " ")

    def walk(items: list):
        for item in items:
            if isinstance(item, WastToken):
                emit(item.text, item.line)
            else:
                emit("(", item.open_line)
                walk(item.items)
                emit(")", item.close_line)

    walk(items)
    return "".join(output)


def make_description(input_path: Path, name: str, out_path: Path) -> WastDescription:
    out_json_path = out_path / f"{name}.json"
    text = input_path.read_text()
    unfolded_path = None
    if "(do" in text:
        items, _ = parse_wast_tokens(tokenize_wast(text))
        unfolded_path = out_path / f"{name}.wast"
        unfolded_path.write_text(serialize_wast(unfold_legacy_try(items)))
    result = subprocess.run(
        [
            "wasm-tools",
            "json-from-wast",
            unfolded_path or input_path,
            "-o",
            out_json_path,
            "--wasm-dir",
            str(out_path),
        ],
    )
    if unfolded_path:
        unfolded_path.unlink()
    result.check_returncode()
    with open(out_json_path, "r") as f:
        description = json.load(f)
    return parse(description)


def to_vector_element(value: str, bits: int, addition: str) -> str:
    if value.isdigit():
        return value + addition
    if value.startswith("-") and value[1:].isdigit():
        unsigned_value = (1 << bits) + int(value)
        return str(unsigned_value) + addition
    return f'"{value}"'


def gen_vector(vec: WasmVector, *, array=False) -> str:
    addition = "n" if vec.num_bits == 64 else ""
    vals = ", ".join(to_vector_element(v, vec.num_bits, addition) for v in vec.lanes)
    if not array:
        type_ = "BigUint64Array" if vec.num_bits == 64 else f"Uint{vec.num_bits}Array"
        return f"new {type_}([{vals}])"
    return f"[{vals}]"


def gen_value_arg(value: WasmValue) -> str:
    if isinstance(value, WasmGCValue):
        if value.value is None or value.value == "null":
            return "null"
        # A host reference (e.g. `(ref.host 2)` passed as anyref); the harness passes host
        # references by their address.
        return value.value

    if isinstance(value, WasmVector):
        return gen_vector(value)

    if isinstance(value, EitherOf):
        raise AssertionError("EitherOf should not appear here")

    if value.value is None:
        raise GenerateException("Cannot generate an argument without a concrete value")

    def unsigned_to_signed(uint: int, bits: int) -> int:
        max_value = 2**bits
        if uint >= 2 ** (bits - 1):
            signed_int = uint - max_value
        else:
            signed_int = uint

        return signed_int

    def int_to_float_bitcast(uint: int) -> float:
        b = struct.pack("I", uint)
        f = struct.unpack("f", b)[0]
        return f

    def int_to_float64_bitcast(uint: int) -> float:
        uint64 = uint & 0xFFFFFFFFFFFFFFFF
        b = struct.pack("Q", uint64)
        f = struct.unpack("d", b)[0]
        return f

    def float_to_str(bits: int, *, double=False) -> str:
        f = int_to_float64_bitcast(bits) if double else int_to_float_bitcast(bits)
        return str(f)

    if value.value.startswith("nan"):
        raise GenerateException("Should not get indeterminate nan value as an argument")
    if value.value == "inf":
        return "Infinity"
    if value.value == "-inf":
        return "-Infinity"

    if value.kind == "i32":
        return str(unsigned_to_signed(int(value.value), 32))
    if value.kind == "i64":
        return str(unsigned_to_signed(int(value.value), 64)) + "n"
    if value.kind == "f32":
        return str(int(value.value)) + f" /* {float_to_str(int(value.value))} */"
    if value.kind == "f64":
        return str(int(value.value)) + f"n /* {float_to_str(int(value.value), double=True)} */"
    if value.kind in ("externref", "funcref", "v128"):
        return value.value
    raise GenerateException(f"Not implemented: {value.kind}")


def gen_value_result(value: WasmValue) -> GeneratedValue:
    if isinstance(value, WasmVector):
        return GeneratedVector(gen_vector(value, array=True), value.num_bits)

    if isinstance(value, EitherOf):
        return GeneratedEitherOf([gen_value_result(option) for option in value.options])

    if isinstance(value, WasmGCValue):
        if value.value == "null":
            return "null"
        # A bottom-type reference (e.g. `(ref.null none)`) can only ever be null.
        if value.kind in ("nullref", "nullexnref", "nullexternref"):
            return "null"
        if value.value is None:
            return GeneratedAnyGCRef(value.kind)
        # A host reference result, surfaced by its address (e.g. `(ref.host 1)` as anyref).
        return value.value

    if value.kind == "funcref" and value.value is None:
        return GeneratedAnyFuncRef()

    if value.kind == "externref" and value.value is None:
        return GeneratedAnyExternRef()

    if value.kind == "f32" or value.kind == "f64":
        assert value.value is not None
        if value.value.startswith("nan"):
            num_bits = int(value.kind[1:])
            if value.value == "nan:canonical":
                return CanonicalNan(num_bits)
            if value.value == "nan:arithmetic":
                return ArithmeticNan(num_bits)
            raise GenerateException(f"Unknown indeterminate nan: {value.value}")
    return gen_value_arg(value)


def gen_args(args: list[WasmValue]) -> str:
    return ",".join(gen_value_arg(arg) for arg in args)


def gen_test_command_for_module(file_name):
    if str(file_name) in TEST_MODULES_TO_SKIP:
        return "_test.skip"
    return "_test"


def gen_test_command_for_invoke(module_name):
    if module_name in TESTS_TO_SKIP:
        return "_test.skip"
    return "_test"


def gen_module_command(command: ModuleCommand, ctx: Context):
    if ctx.has_unclosed:
        print("});")
    print(
        f"""describe("{command.file_name.stem}", () => {{
let _test = test;
let content, module;
try {{
content = readBinaryWasmFile("Fixtures/SpecTests/{command.file_name}");
module = parseWebAssemblyModule(content, globalImportObject);
}} catch (e) {{
{gen_test_command_for_module(command.file_name)}("parse (line {command.line})", () => expect().fail(e));
_test = test.skip;
_test.skip = test.skip;
}}
"""
    )
    if command.name is not None:
        print(f'namedModules["{command.name}"] = module;')
    ctx.current_module_name = command.file_name.stem
    ctx.has_unclosed = True


def gen_module_definition_command(command: ModuleDefinitionCommand, ctx: Context):
    if ctx.has_unclosed:
        print("});")
        ctx.has_unclosed = False
    stem = command.file_name.stem
    print(
        f"""
describe("{stem}", () => {{
let _test = test;
{gen_test_command_for_module(command.file_name)}("validate (line {command.line})", () => {{
content = readBinaryWasmFile("Fixtures/SpecTests/{command.file_name}");
validateWebAssemblyModule(content);
}});
}});"""
    )


def gen_invalid(invalid: AssertInvalid, ctx: Context):
    # TODO: Remove this once the multiple memories proposal is standardized.
    # We support the multiple memories proposal, so spec-tests that check that
    # we don't do not make any sense to include right now.
    if invalid.message == "multiple memories":
        return
    if ctx.has_unclosed:
        print("});")
        ctx.has_unclosed = False
    stem = invalid.filename.stem
    print(
        f"""
describe("{stem}", () => {{
let _test = test;
{gen_test_command_for_module(invalid.filename)}("parse of {stem} (line {invalid.line})", () => {{
content = readBinaryWasmFile("Fixtures/SpecTests/{invalid.filename}");
expect(() => parseWebAssemblyModule(content, globalImportObject)).toThrow(Error, "{invalid.message}");
}});
}});"""
    )


def gen_pretty_expect(expr: str, got: str, expect: str):
    print(f"if (!({expr})) {{ expect().fail(`Failed with ${{{got}}}, expected {expect}`); }}")


def gen_expectation(gen_result: GeneratedValue, module: str):
    if isinstance(gen_result, str):
        print(f"expect(_result).toBe({gen_result});")
        return
    if isinstance(gen_result, GeneratedAnyFuncRef):
        print(f"/* {gen_result} */ ", end="")
        gen_pretty_expect(
            f"isValidFuncrefIn(_result, {module})",
            "_result",
            "(ref.func)",
        )
        return
    if isinstance(gen_result, GeneratedAnyExternRef):
        # Null extern references surface as JS null; live ones as their address.
        print(f"/* {gen_result} */ ", end="")
        gen_pretty_expect(
            "_result !== null",
            "_result",
            "(ref.extern)",
        )
        return
    if isinstance(gen_result, GeneratedAnyGCRef):
        # Null gc references surface as JS null; the harness cannot check the heap type of a
        # live one, only that it is not null.
        print(f"/* {gen_result} */ ", end="")
        gen_pretty_expect(
            "_result !== null",
            "_result",
            f"(ref.{gen_result.kind})",
        )
        return
    if isinstance(gen_result, ArithmeticNan):
        print(f"/* {gen_result} */ ", end="")
        gen_pretty_expect(
            f"isArithmeticNaN{gen_result.num_bits}(_result)",
            "_result",
            "nan:arithmetic",
        )
        return
    if isinstance(gen_result, CanonicalNan):
        print(f"/* {gen_result} */ ", end="")
        gen_pretty_expect(
            f"isCanonicalNaN{gen_result.num_bits}(_result)",
            "_result",
            "nan:canonical",
        )
        return
    if isinstance(gen_result, GeneratedVector):
        if gen_result.num_bits == 64:
            array = "new BigUint64Array(_result)"
        else:
            array = f"new Uint{gen_result.num_bits}Array(_result)"
        print(f"/* {gen_result} */ ", end="")
        gen_pretty_expect(
            f"testSIMDVector({gen_result.repr}, {array})",
            array,
            gen_result.repr,
        )
        return
    assert isinstance(gen_result, GeneratedEitherOf)
    print("let matched = false;")
    print("let error_sample = null;")
    expectations = []
    for option in gen_result.options:
        print("try {")
        gen_expectation(option, module)
        print("matched = true;")
        print("} catch (e) { error_sample = e; }")
        expectation = "unknown"
        if isinstance(option, str):
            expectation = option
        elif isinstance(option, ArithmeticNan):
            expectation = "nan:arithmetic"
        elif isinstance(option, CanonicalNan):
            expectation = "nan:canonical"
        elif isinstance(option, GeneratedVector):
            expectation = option.repr
        expectations.append(expectation)
    print(
        f"if (!matched) {{ expect().fail(`Expected one of {', '.join(expectations)}, got ${{_result}}: ${{error_sample}}`); }}"
    )


def gen_invoke(
    line: int,
    invoke: Invoke,
    result: Optional[WasmValue],
    ctx: Context,
    *,
    fail_msg: Optional[str] = None,
):
    if not ctx.has_unclosed:
        print(f'describe("inline (line {line}))", () => {{\nlet _test = test;\n')
    module = "module"
    if invoke.module is not None:
        module = f'namedModules["{invoke.module}"]'
    utf8 = str(invoke.field.encode("utf8"))[2:-1].replace("\\'", "'").replace("`", "${'`'}")
    print(
        f"""{gen_test_command_for_invoke(ctx.current_module_name)}(`execution of {ctx.current_module_name}: {utf8} (line {line})`, () => {{
let _field = {module}.getExport(decodeURIComponent(escape(`{utf8}`)));
expect(_field).not.toBeUndefined();"""
    )
    if fail_msg is not None:
        print(f'expect(() => {module}.invoke(_field)).toThrow(Error, "{fail_msg}");')
    else:
        print(f"let _result = {module}.invoke(_field, {gen_args(invoke.args)});")
    if result is not None:
        gen_result = gen_value_result(result)
        gen_expectation(gen_result, module)
    print("});")
    if not ctx.has_unclosed:
        print("});")


def gen_get(line: int, get: Get, result: Optional[WasmValue], ctx: Context):
    module = "module"
    if get.module is not None:
        module = f'namedModules["{get.module}"]'
    print(
        f"""{gen_test_command_for_invoke(ctx.current_module_name)}("execution of {ctx.current_module_name}: get-{get.field} (line {line})", () => {{
let _field = {module}.getExport("{get.field}");"""
    )
    if result is not None:
        print(f"expect(_field).toBe({gen_value_result(result)});")
    print("});")


def gen_register(register: Register, _: Context):
    module = "module"
    if register.name is not None:
        module = f'namedModules["{register.name}"]'
    print(f'globalImportObject["{register.as_}"] = {module};')


def gen_command(command: Command, ctx: Context):
    if isinstance(command, ModuleCommand):
        gen_module_command(command, ctx)
        return
    if isinstance(command, ModuleDefinitionCommand):
        gen_module_definition_command(command, ctx)
        return
    if isinstance(command, ActionCommand):
        if isinstance(command.action, Invoke):
            gen_invoke(command.line, command.action, None, ctx)
        else:
            raise GenerateException(f"Not implemented: top-level {type(command.action)}")
        return
    if isinstance(command, AssertInvalid):
        gen_invalid(command, ctx)
        return
    if isinstance(command, Register):
        gen_register(command, ctx)
        return
    if isinstance(command, AssertReturn):
        if isinstance(command.action, Invoke):
            gen_invoke(command.line, command.action, command.expected, ctx)
        else:
            gen_get(command.line, command.action, command.expected, ctx)
        return
    if isinstance(command, AssertTrap):
        if not isinstance(command.action, Invoke):
            raise GenerateException(f"Not implemented: {type(command.action)}")
        gen_invoke(command.line, command.action, None, ctx, fail_msg=command.messsage)
        return
    assert isinstance(command, AssertException)
    if not isinstance(command.action, Invoke):
        raise GenerateException(f"Not implemented: {type(command.action)}")
    gen_invoke(command.line, command.action, None, ctx, fail_msg="exception")


def generate(description: WastDescription):
    print("let globalImportObject = {};\nlet namedModules = {};\n")
    ctx = Context("", False)
    for command in description.commands:
        gen_command(command, ctx)
    if ctx.has_unclosed:
        print("});")


def clean_up(path: Path):
    for file in path.iterdir():
        if file.suffix in ("wat", "json"):
            file.unlink()


def main():
    input_path = Path(sys.argv[1])
    name = sys.argv[2]
    out_path = Path(sys.argv[3])

    description = make_description(input_path, name, out_path)
    generate(description)
    clean_up(out_path)


if __name__ == "__main__":
    main()
