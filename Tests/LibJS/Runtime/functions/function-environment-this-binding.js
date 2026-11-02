// Functions whose code never reads `this`, `new.target` or `super` through their environment get a lighter environment
// for their captured bindings. Everything that does read them has to keep working.

const ITERATIONS = 5000;

function repeated(callback) {
    let result;
    for (let i = 0; i < ITERATIONS; ++i) result = callback(i);
    return result;
}

test("closures that do not read this through their environment", () => {
    function counter(start) {
        let count = start;
        return () => ++count;
    }
    const next = repeated(i => counter(i));
    expect(next()).toBe(ITERATIONS);
    expect(next()).toBe(ITERATIONS + 1);
});

test("this read directly next to captured bindings", () => {
    function method(value) {
        const captured = () => value;
        return [this.name, captured()];
    }
    const object = { name: "object", method };
    expect(repeated(i => object.method(i))).toEqual(["object", ITERATIONS - 1]);

    function sloppy() {
        const value = 1;
        const captured = () => value;
        captured();
        return typeof this;
    }
    expect(repeated(() => sloppy.call(1))).toBe("object");
    expect(repeated(() => sloppy())).toBe("object");
});

test("this read through arrow functions", () => {
    function method(value) {
        return () => () => [this.name, value];
    }
    const object = { name: "object", method };
    expect(repeated(i => object.method(i)()())).toEqual(["object", ITERATIONS - 1]);
    expect(object.method(1)()()).toEqual(["object", 1]);
});

test("arrow functions with captured bindings of their own", () => {
    function method() {
        return value => {
            const captured = () => value;
            return [this.name, captured()];
        };
    }
    const object = { name: "object", method };
    expect(repeated(i => object.method()(i))).toEqual(["object", ITERATIONS - 1]);

    const lexical = value => {
        const captured = () => value;
        return captured();
    };
    expect(repeated(i => lexical(i))).toBe(ITERATIONS - 1);
});

test("new.target next to captured bindings", () => {
    function Direct(value) {
        const captured = () => value;
        this.value = captured();
        this.target = new.target;
    }
    expect(repeated(i => new Direct(i)).target).toBe(Direct);
    expect(Direct.call({}, 1)).toBeUndefined();

    function ThroughArrow(value) {
        const captured = () => [new.target, value];
        this.result = captured();
    }
    expect(repeated(i => new ThroughArrow(i)).result).toEqual([ThroughArrow, ITERATIONS - 1]);
});

test("super next to captured bindings", () => {
    class Base {
        constructor(value) {
            this.base = value;
        }
        method() {
            return "base";
        }
    }
    class Derived extends Base {
        constructor(value) {
            const captured = () => value;
            super(captured());
            this.derived = captured();
        }
        method(value) {
            const captured = () => value;
            return [super.method(), captured()];
        }
        arrow(value) {
            return (() => [super.method(), value])();
        }
    }
    class ThroughArrow extends Base {
        constructor(value) {
            const call = () => super(value);
            call();
            this.after = value;
        }
    }
    const object = repeated(i => new Derived(i));
    expect(object.base).toBe(ITERATIONS - 1);
    expect(object.derived).toBe(ITERATIONS - 1);
    expect(repeated(i => object.method(i))).toEqual(["base", ITERATIONS - 1]);
    expect(repeated(i => object.arrow(i))).toEqual(["base", ITERATIONS - 1]);
    expect(repeated(i => new ThroughArrow(i)).base).toBe(ITERATIONS - 1);

    class Uninitialized extends Base {
        constructor() {
            const read = () => this;
            expect(read).toThrow(ReferenceError);
            super(1);
            expect(read().base).toBe(1);
        }
    }
    new Uninitialized();
});

test("direct eval reads everything", () => {
    function evaluates(value) {
        const captured = () => value;
        captured();
        return [eval("this.name"), eval("new.target"), eval("value")];
    }
    expect(evaluates.call({ name: "object" }, 1)).toEqual(["object", undefined, 1]);

    function evaluatesInArrow(value) {
        return (() => [eval("this.name"), eval("new.target"), eval("value")])();
    }
    expect(evaluatesInArrow.call({ name: "object" }, 1)).toEqual(["object", undefined, 1]);

    function nested(value) {
        const inner = () => eval("var added = value + 1; added");
        return inner();
    }
    expect(nested(1)).toBe(2);

    class Base {
        method() {
            return "base";
        }
    }
    class Derived extends Base {
        method() {
            const value = 1;
            return () => eval("super.method()") + value;
        }
    }
    expect(new Derived().method()()).toBe("base1");
});

test("generators and async functions with captured bindings", () => {
    function* generator(value) {
        const captured = () => value;
        yield this.name;
        yield captured();
        yield (() => this.name)();
    }
    expect([...generator.call({ name: "object" }, 1)]).toEqual(["object", 1, "object"]);

    let result;
    async function asynchronous(value) {
        const captured = () => value;
        await null;
        result = [this.name, captured(), (() => this.name)()];
    }
    asynchronous.call({ name: "object" }, 1);
    runQueuedPromiseJobs();
    expect(result).toEqual(["object", 1, "object"]);
});

test("arguments objects and parameter scopes", () => {
    function mapped(a) {
        const captured = () => a;
        arguments[0] = 2;
        return [captured(), arguments.length, this.name];
    }
    expect(repeated(() => mapped.call({ name: "object" }, 1))).toEqual([2, 1, "object"]);

    function defaults(a, b = () => a) {
        var a;
        return b();
    }
    expect(repeated(() => defaults(1))).toBe(1);
});

test("class fields and static blocks", () => {
    class Fields {
        value = 1;
        arrow = () => this.value;
        static captured = (() => this.name)();
        static {
            const value = 2;
            const read = () => [this.name, value];
            this.read = read();
        }
    }
    expect(repeated(() => new Fields().arrow())).toBe(1);
    expect(Fields.captured).toBe("Fields");
    expect(Fields.read).toEqual(["Fields", 2]);
});
