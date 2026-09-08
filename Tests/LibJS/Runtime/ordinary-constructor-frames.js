test("ordinary constructors preserve returns, new.target, and exceptions", () => {
    function Assign(a, b) {
        this.total = a + b;
        this.target = new.target;
        gc();
    }
    function Primitive(value) {
        return value;
    }
    function Replacement(value) {
        gc();
        return value;
    }
    function CaptureTarget() {
        return () => new.target;
    }
    function Throwing(value) {
        throw value;
    }
    function Prototype() {}
    class Derived extends Assign {}
    const Bound = Assign.bind(null, 10);
    const replacement = { replacement: true };

    for (let iteration = 0; iteration < 3; ++iteration) {
        const object = new Assign(3, 4);
        expect(object.total).toBe(7);
        expect(object.target).toBe(Assign);
        expect(object).toBeInstanceOf(Assign);

        for (const value of [undefined, null, false, 17, "text", 12n, Symbol()])
            expect(new Primitive(value)).toBeInstanceOf(Primitive);

        expect(new Replacement(replacement)).toBe(replacement);
        expect(new CaptureTarget()()).toBe(CaptureTarget);

        const derived = new Derived(4, 5);
        expect(derived.total).toBe(9);
        expect(derived.target).toBe(Derived);
        expect(derived).toBeInstanceOf(Derived);

        const bound = new Bound(5);
        expect(bound.total).toBe(15);
        expect(bound.target).toBe(Assign);

        const reflected = Reflect.construct(Assign, [6, 7], Derived);
        expect(reflected.total).toBe(13);
        expect(reflected.target).toBe(Derived);
        expect(reflected).toBeInstanceOf(Derived);

        expect(() => new Throwing(replacement)).toThrow(replacement);

        new Prototype();
        Prototype.prototype = null;
        expect(Object.getPrototypeOf(new Prototype())).toBe(Object.prototype);
        Prototype.prototype = {};
    }
});
