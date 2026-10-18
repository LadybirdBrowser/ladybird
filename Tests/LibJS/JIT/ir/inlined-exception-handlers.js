// Callees with exception handlers, like the handler that closes the iterator of a for-of loop, are inlined if none of
// their handlers caught an exception before the compile. Their handlers get no blocks. Callees whose handler caught
// stay calls.

class Writer {
    constructor() {
        this.values = [0, 0, 0, 0, 0, 0, 0, 0];
        this.position = 0;
    }
    write(list) {
        for (const value of list) this.values[this.position++] = value;
    }
}
function writeAll(writer, list) {
    writer.position = 0;
    writer.write(list);
    return writer.position;
}
const writer = new Writer();
jit.prepare(Writer.prototype.write);
jit.prepare(writeAll);
for (let i = 0; i < 10; ++i) writeAll(writer, [1, 2, 3]);
jit.compile(writeAll);

// CHECK-FUNCTION: writeAll
// CHECK-PASS: frame initialization
// CHECK-NOT: CallDirect
// CHECK: LoadElementAt Holey
// CHECK-NOT: CallDirect

function parsed(value) {
    try {
        if (typeof value !== "number") throw value;
        return value + 1;
    } catch (e) {
        return -1;
    }
}
function sumParsed(a, b) {
    return parsed(a) + parsed(b);
}
jit.prepare(parsed);
jit.prepare(sumParsed);
for (let i = 0; i < 10; ++i) sumParsed(i, i);
jit.compile(sumParsed);

// CHECK-FUNCTION: sumParsed
// CHECK-PASS: frame initialization
// CHECK-NOT: CallDirect
// CHECK: Int32Add
// CHECK-NOT: CallDirect

function caught(value) {
    try {
        if (typeof value !== "number") throw value;
        return value + 1;
    } catch (e) {
        return -1;
    }
}
function sumCaught(a, b) {
    return caught(a) + caught(b);
}
jit.prepare(caught);
jit.prepare(sumCaught);
for (let i = 0; i < 10; ++i) sumCaught(i, "x");
jit.compile(sumCaught);

// CHECK-FUNCTION: sumCaught
// CHECK-PASS: frame initialization
// CHECK: CallDirect
