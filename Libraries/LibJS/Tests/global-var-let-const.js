var foo = 1;
let bar = 2;
const baz = 3;

test("behavior of program-level var/let/const", () => {
    expect(foo).toBe(1);
    expect(bar).toBe(2);
    expect(baz).toBe(3);
    expect(globalThis.foo).toBe(1);
    expect(globalThis.bar).toBeUndefined();
    expect(globalThis.baz).toBeUndefined();
    globalThis.foo = 4;
    globalThis.bar = 5;
    globalThis.baz = 6;
    expect(foo).toBe(4);
    expect(bar).toBe(2);
    expect(baz).toBe(3);
    expect(globalThis.foo).toBe(4);
    expect(globalThis.bar).toBe(5);
    expect(globalThis.baz).toBe(6);
});
