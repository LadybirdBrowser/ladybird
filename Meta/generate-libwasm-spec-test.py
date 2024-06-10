import json
import math
import sys
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Union, Literal, Any


class ParseException(Exception):
    pass


class GenerateException(Exception):
    pass


@dataclass
class WasmValue:
    kind: Literal["i32", "i64", "f32", "f64", "externref", "funcref", "v128"]
    value: str


@dataclass
class ModuleCommand:
    line: int
    file_name: Path
    name: str | None


@dataclass
class Invoke:
    field: str
    args: list[WasmValue]
    module: str | None


@dataclass
class Get:
    field: str
    module: str | None


Action = Union[Invoke, Get]


@dataclass
class Register:
    line: int
    name: str | None
    as_: str


@dataclass
class AssertReturn:
    line: int
    action: Action
    expected: WasmValue | None


@dataclass
class AssertTrap:
    line: int
    messsage: str
    action: Action


@dataclass
class ActionCommand:
    line: int
    action: Action


@dataclass
class AssertInvalid:
    line: int
    filename: str
    message: str


Command = Union[
    ModuleCommand,
    AssertReturn,
    AssertTrap,
    ActionCommand,
    AssertInvalid,
    Register,
]


@dataclass
class WastDescription:
    source_filename: str
    commands: list[Command]


@dataclass
class Context:
    current_module_name: str
    has_unclosed: bool


def parse_value(arg: dict[str, str]) -> WasmValue:
    type_ = arg["type"]
    match type_:
        case "i32" | "i64" | "f32" | "f64" | "externref" | "funcref":
            payload = arg["value"]
        case "v128":

            def reverse_endianness(hex_str):
                if len(hex_str) % 2 != 0:
                    hex_str = "0" + hex_str
                bytes_list = [hex_str[i:i + 2] for i in range(0, len(hex_str), 2)]
                reversed_hex_str = "".join(bytes_list[::-1])
                return reversed_hex_str

            size = int(arg["lane_type"][1:]) // 4
            parts = []
            for raw_val in arg["value"]:
                match raw_val:
                    case "nan:canonical":
                        hex_repr = "7fc".ljust(size, "0")
                    case "nan:arithmetic":
                        hex_repr = "7ff".ljust(size, "0")
                    case "nan:signaling":
                        hex_repr = "7ff8".ljust(size, "0")
                    case _:
                        hex_repr = hex(int(raw_val))[2:].zfill(size)
                parts.append(hex_repr)
            payload = "0x" + reverse_endianness("".join(reversed(parts))) + "n"
        case _:
            raise ParseException(f"Unknown value type: {type_}")
    return WasmValue(type_, payload)


def parse_args(raw_args: list[dict[str, str]]) -> list[WasmValue]:
    return [parse_value(arg) for arg in raw_args]


def parse_action(action: dict[str, Any]) -> Action:
    match action["type"]:
        case "invoke":
            return Invoke(
                action["field"], parse_args(action["args"]), action.get("module")
            )
        case "get":
            return Get(action["field"], action.get("module"))
        case _:
            raise ParseException(f"Action not implemented: {action['type']}")


def parse(raw: dict[str, Any]) -> WastDescription:
    commands: list[Command] = []
    for raw_cmd in raw["commands"]:
        line = raw_cmd["line"]
        cmd: Command
        match raw_cmd["type"]:
            case "module":
                cmd = ModuleCommand(
                    line, Path(raw_cmd["filename"]), raw_cmd.get("name")
                )
            case "action":
                cmd = ActionCommand(line, parse_action(raw_cmd["action"]))
            case "register":
                cmd = Register(line, raw_cmd.get("name"), raw_cmd["as"])
            case "assert_return":
                cmd = AssertReturn(
                    line,
                    parse_action(raw_cmd["action"]),
                    parse_value(raw_cmd["expected"][0])
                    if len(raw_cmd["expected"]) == 1
                    else None,
                )
            case "assert_trap" | "assert_exhaustion":
                cmd = AssertTrap(line, raw_cmd["text"], parse_action(raw_cmd["action"]))
            case "assert_invalid" | "assert_malformed" | "assert_uninstantiable" | "assert_unlinkable":
                if raw_cmd.get("module_type") == "text":
                    continue
                cmd = AssertInvalid(line, raw_cmd["filename"], raw_cmd["text"])
            case _:
                raise ParseException(f"Unknown command type: {raw_cmd['type']}")
        commands.append(cmd)

    return WastDescription(raw["source_filename"], commands)


def escape(s: str) -> str:
    return s.replace('"', '\\"')


def make_description(input_path: Path, name: str, out_path: Path) -> WastDescription:
    out_json_path = out_path / f"{name}.json"
    result = subprocess.run(
        ["wast2json", input_path, f"--output={out_json_path}", "--no-check"],
    )
    result.check_returncode()
    with open(out_json_path, "r") as f:
        description = json.load(f)
    return parse(description)


def gen_value(value: WasmValue) -> str:
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

    def float_to_str(f: float) -> str:
        if math.isnan(f) and math.copysign(1.0, f) < 0:
            return "-NaN"
        elif math.isnan(f):
            return "NaN"
        elif math.isinf(f) and math.copysign(1.0, f) < 0:
            return "-Infinity"
        elif math.isinf(f):
            return "Infinity"
        return str(f)

    if value.value.startswith("nan"):
        return "NaN"
    elif value.value.startswith("-nan"):
        return "-NaN"
    elif value.value == "inf":
        return "Infinity"
    elif value.value == "-inf":
        return "-Infinity"

    match value.kind:
        case "i32":
            return str(unsigned_to_signed(int(value.value), 32))
        case "i64":
            return str(unsigned_to_signed(int(value.value), 64)) + "n"
        case "f32":
            return float_to_str(int_to_float_bitcast(int(value.value)))
        case "f64":
            return float_to_str(int_to_float64_bitcast(int(value.value)))
        case "externref" | "funcref" | "v128":
            return value.value
        case _:
            raise GenerateException(f"Not implemented: {value.kind}")


def gen_args(args: list[WasmValue]) -> str:
    return ",".join(gen_value(arg) for arg in args)


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
_test("parse", () => expect().fail(e));
_test = test.skip;
_test.skip = test.skip;
}}
"""
    )
    if command.name is not None:
        print(f'namedModules["{command.name}"] = module;')
    ctx.current_module_name = command.file_name.stem
    ctx.has_unclosed = True


def gen_invalid(invalid: AssertInvalid, ctx: Context):
    if ctx.has_unclosed:
        print("});")
        ctx.has_unclosed = False
    stem = Path(invalid.filename).stem
    print(
        f"""
describe("{stem}", () => {{
let _test = test;
_test("parse of {stem} (line {invalid.line})", () => {{
content = readBinaryWasmFile("Fixtures/SpecTests/{invalid.filename}");
expect(() => parseWebAssemblyModule(content, globalImportObject)).toThrow(Error, "{invalid.message}");
}});
}});"""
    )


def gen_invoke(
    line: int,
    invoke: Invoke,
    result: WasmValue | None,
    ctx: Context,
    *,
    fail_msg: str | None = None,
):
    module = "module"
    if invoke.module is not None:
        module = f'namedModules["{invoke.module}"]'
    print(
        f"""_test("execution of {ctx.current_module_name}: {escape(invoke.field)} (line {line})", () => {{
let _field = {module}.getExport("{escape(invoke.field)}");
expect(_field).not.toBeUndefined();"""
    )
    if fail_msg is not None:
        print(f'expect(() => {module}.invoke(_field)).toThrow(Error, "{fail_msg}");')
    else:
        print(f"let _result = {module}.invoke(_field, {gen_args(invoke.args)});")
    if result is not None:
        print(f"expect(_result).toBe({gen_value(result)});")
    print("});")


def gen_get(line: int, get: Get, result: WasmValue | None, ctx: Context):
    module = "module"
    if get.module is not None:
        module = f'namedModules["{get.module}"]'
    print(
        f"""_test("execution of {ctx.current_module_name}: get-{get.field} (line {line})", () => {{
let _field = {module}.getExport("{get.field}");"""
    )
    if result is not None:
        print(f"expect(_field).toBe({gen_value(result)});")
    print("});")


def gen_register(register: Register, _: Context):
    module = "module"
    if register.name is not None:
        module = f'namedModules["{register.name}"]'
    print(f'globalImportObject["{register.as_}"] = {module};')


def gen_command(command: Command, ctx: Context):
    match command:
        case ModuleCommand():
            gen_module_command(command, ctx)
        case ActionCommand():
            if isinstance(command.action, Invoke):
                gen_invoke(command.line, command.action, None, ctx)
            else:
                raise GenerateException(
                    f"Not implemented: top-level {type(command.action)}"
                )
        case AssertInvalid():
            gen_invalid(command, ctx)
        case Register():
            gen_register(command, ctx)
        case AssertReturn():
            match command.action:
                case Invoke():
                    gen_invoke(command.line, command.action, command.expected, ctx)
                case Get():
                    gen_get(command.line, command.action, command.expected, ctx)
        case AssertTrap():
            if not isinstance(command.action, Invoke):
                raise GenerateException(f"Not implemented: {type(command.action)}")
            gen_invoke(
                command.line, command.action, None, ctx, fail_msg=command.messsage
            )


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
