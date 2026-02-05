let haveSpecTestSuite = false;
try {
    readBinaryWasmFile("Fixtures/SpecTests/address.wasm");
    haveSpecTestSuite = true;
} catch {}

let testFunction = haveSpecTestSuite ? test : test.skip;

// prettier-ignore
const tests = [
    "address", "align", "binary", "binary-leb128", "br_table", "comments", "endianness", "exports",
    "f32", "f32_bitwise", "f32_cmp", "f64", "f64_bitwise", "f64_cmp", "float_exprs", "float_literals",
    "float_memory", "float_misc", "forward", "func_ptrs", "int_exprs", "int_literals", "labels",
    "left-to-right", "linking", "load", "local_get", "memory", "memory_grow", "memory_redundancy",
    "memory_size", "memory_trap", "names", "return", "switch", "table", "traps", "type"
];

for (let testName of tests) {
    testFunction(`parse ${testName}`, () => {
        const contents = readBinaryWasmFile(`Fixtures/SpecTests/${testName}.wasm`);
        parseWebAssemblyModule(contents);
    });
}
