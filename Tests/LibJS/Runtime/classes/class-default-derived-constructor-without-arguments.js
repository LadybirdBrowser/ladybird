test("a default derived constructor called without arguments in a hot loop", () => {
    class Base {
        constructor(...args) {
            this.count = args.length;
        }
    }
    class Derived extends Base {}
    let total = 0;
    for (let i = 0; i < 20000; ++i) {
        total += new Derived().count;
    }
    expect(total).toBe(0);
    expect(new Derived(1, 2).count).toBe(2);
});

test("a default derived constructor of a builtin called without arguments in a hot loop", () => {
    class Derived extends Uint8Array {}
    let total = 0;
    for (let i = 0; i < 20000; ++i) {
        total += new Derived().length;
    }
    expect(total).toBe(0);
    expect(new Derived(3).length).toBe(3);
});
