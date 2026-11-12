// Bytecode holds the index of a function and the slot of an object literal property in 16 bits. Past that, the code
// takes another way, which must give the same results.

test("functions of an executable past the first 65536", () => {
    const count = 66000;
    let source = "return [";
    for (let i = 0; i < count; ++i) source += `() => ${i},`;
    source +=
        "function named() { return 'named'; }, { method() { return super.toString === Object.prototype.toString; } }];";
    const functions = new Function(source)();
    expect(functions).toHaveLength(count + 2);
    for (const i of [0, 1, 65534, 65535, 65536, 65537, count - 1]) {
        expect(functions[i]()).toBe(i);
    }
    expect(functions[count]()).toBe("named");
    expect(functions[count].name).toBe("named");
    expect(functions[count + 1].method()).toBeTrue();
});

test("properties of an object literal past the first 65536", () => {
    const count = 66000;
    let source = "return {";
    for (let i = 0; i < count; ++i) source += `p${i}: ${i},`;
    source += "};";
    const make = new Function(source);
    // NB: The second and later literals start out with the shape the first one ended up with.
    for (let run = 0; run < 3; ++run) {
        const object = make();
        const keys = Object.keys(object);
        expect(keys).toHaveLength(count);
        for (const i of [0, 1, 65534, 65535, 65536, 65537, count - 1]) {
            expect(keys[i]).toBe(`p${i}`);
            expect(object[`p${i}`]).toBe(i);
        }
    }
});

test("object literals of an executable past the last shape cache slot", () => {
    const count = 66000;
    let source = "const objects = [];";
    for (let i = 0; i < count; ++i) source += `objects.push({ a: ${i}, b: ${i + 1} });`;
    source += "return objects;";
    const make = new Function(source);
    for (let run = 0; run < 3; ++run) {
        const objects = make();
        for (const i of [0, 65534, 65535, 65536, count - 1]) {
            expect(Object.keys(objects[i])).toEqual(["a", "b"]);
            expect(objects[i].a).toBe(i);
            expect(objects[i].b).toBe(i + 1);
        }
    }
});
