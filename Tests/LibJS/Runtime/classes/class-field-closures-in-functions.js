test("a closure in a field captures a local of a function with a using declaration", () => {
    function run() {
        using resource = null;
        let base = 1;
        class C {
            static fromField = base + 1;
            field = () => base + C.fromField;
            static {
                this.fromBlock = () => base;
            }
        }
        base = 10;
        return [new C().field(), C.fromBlock()];
    }
    expect(run()).toEqual([12, 10]);
});

test("a closure nested deeper in a field captures a local as well", () => {
    function run() {
        using resource = null;
        const value = 1;
        class C {
            field = () => () => {
                try {
                    value++;
                } catch {
                    return value;
                }
            };
        }
        return new C().field()();
    }
    expect(run()).toBe(1);
});
