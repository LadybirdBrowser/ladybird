test("no arguments", () => {
    let getNumber = () => {
        return 42;
    };
    expect(getNumber()).toBe(42);

    getNumber = () => 42;
    expect(getNumber()).toBe(42);

    getNumber = () => {
        return 99;
    };
    expect(getNumber()).toBe(99);
    getNumber = () => 99;
    expect(getNumber()).toBe(99);
});

test("arguments", () => {
    let add = (a, b) => a + b;
    expect(add(2, 3)).toBe(5);

    const addBlock = (a, b) => {
        let res = a + b;
        return res;
    };
    expect(addBlock(5, 4)).toBe(9);
});

test("inside an array", () => {
    let chompy = [x => x, 2];
    expect(chompy).toHaveLength(2);
    expect(chompy[0](1)).toBe(1);
});

test("return object literal", () => {
    const makeObject = (a, b) => ({ a, b });
    const obj = makeObject(33, 44);
    expect(typeof obj).toBe("object");
    expect(obj.a).toBe(33);
    expect(obj.b).toBe(44);
});

test("return undefined", () => {
    let returnUndefined = () => {};
    expect(returnUndefined()).toBeUndefined();
});

test("return array literal", () => {
    const makeArray = (a, b) => [a, b];
    const array = makeArray("3", { foo: 4 });
    expect(array[0]).toBe("3");
    expect(array[1].foo).toBe(4);
});

test("return numeric expression", () => {
    let square = x => x * x;
    expect(square(3)).toBe(9);

    let squareBlock = x => {
        return x * x;
    };
    expect(squareBlock(4)).toBe(16);
});

test("return called arrow function expression", () => {
    const message = (who => "Hello " + who)("friends!");
    expect(message).toBe("Hello friends!");

    const sum = ((x, y, z) => x + y + z)(1, 2, 3);
    expect(sum).toBe(6);

    const product = ((x, y, z) => {
        let res = x * y * z;
        return res;
    })(5, 4, 2);
    expect(product).toBe(40);

    const half = (x => {
        return x / 2;
    })(10);
    expect(half).toBe(5);
});

test("currying", () => {
    let add = a => b => a + b;
    expect(typeof add(1)).toBe("function");
    expect(typeof add(1, 2)).toBe("function");
    expect(add(1)(2)).toBe(3);
});

test("with comma operator", () => {
    let foo, bar;
    (foo = bar), baz => {};
    expect(foo).toBe(undefined);
    expect(bar).toBe(undefined);
});

test("arrow functions in objects", () => {
    function FooBar() {
        this.x = {
            y: () => this,
            z: function () {
                return (() => this)();
            },
        };
    }

    const foobar = new FooBar();
    expect(foobar.x.y()).toBe(foobar);
    expect(foobar.x.z()).toBe(foobar.x);
});

test("strict mode propagation", () => {
    (() => {
        "use strict";
        expect(isStrictMode()).toBeTrue();

        (() => {
            expect(isStrictMode()).toBeTrue();
        })();
    })();

    (() => {
        "use strict";
        expect(isStrictMode()).toBeTrue();
    })();

    (() => {
        expect(isStrictMode()).toBeFalse();

        (() => {
            "use strict";
            expect(isStrictMode()).toBeTrue();
        })();

        expect(isStrictMode()).toBeFalse();
    })();
});

test("no prototype", () => {
    let foo = () => {};
    expect(foo).not.toHaveProperty("prototype");
});

test("cannot be constructed", () => {
    let foo = () => {};
    expect(() => {
        new foo();
    }).toThrowWithMessage(
        TypeError,
        "[object ECMAScriptFunctionObject] is not a constructor (evaluated from 'foo')"
    );
});

test("syntax errors", () => {
    expect("a, => {}").not.toEval();
    expect("(a, => {}").not.toEval();
    expect("(,) => {}").not.toEval();
    expect("(,,) => {}").not.toEval();
    expect("(a,,) => {}").not.toEval();
    expect("(a,,b) => {}").not.toEval();
    expect("(a, ...b, ...c) => {}").not.toEval();
    expect("(a b) => {}").not.toEval();
    expect("(a ...b) => {}").not.toEval();
    expect("(a = 1 = 2) => {}").not.toEval();
    expect("()\n=> {}").not.toEval();
});

test("destructuring parameters", () => {
    expect(`({ a }) => {}`).toEval();
    expect(`([ a ]) => {}`).toEval();
    expect(`{ a } => {}`).not.toEval();
    expect(`[ a ] => {}`).not.toEval();
});
