// Some shapes gain properties in place, like those of functions creating their prototype
// lazily. Property caches remember the dictionary generation of every shape, so that
// their entries stop applying when that happens.

const readName = value => value.name;
const readLength = value => value.length;
const readMissing = value => value.missing;

test("own properties of functions before and after their prototype is created", () => {
    for (let round = 0; round < 50; ++round) {
        const functions = [function first(a) {}, function second(a, b) {}];
        expect(functions.map(readName)).toEqual(["first", "second"]);
        expect(functions.map(readMissing)).toEqual([undefined, undefined]);
        expect(functions[0].prototype.constructor).toBe(functions[0]);
        expect(functions.map(readName)).toEqual(["first", "second"]);
        expect(functions.map(readLength)).toEqual([1, 2]);
        expect(functions.map(readMissing)).toEqual([undefined, undefined]);
        functions[1].missing = "added";
        expect(functions.map(readMissing)).toEqual([undefined, "added"]);
    }
});

test("writes to own properties of functions", () => {
    function counter() {}
    counter.count = 0;
    const increment = target => {
        target.count = target.count + 1;
    };
    for (let i = 0; i < 100; ++i) increment(counter);
    expect(counter.prototype.constructor).toBe(counter);
    for (let i = 0; i < 100; ++i) increment(counter);
    expect(counter.count).toBe(200);
});
