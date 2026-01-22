// Note the globalThisValue and globalObject do not need to be the same.
const globalThisValue = this;
const globalObject = (0, eval)("this");

// These tests are done in global state to ensure that is possible
const globalArrow = () => {
    expect(this).toBe(globalThisValue);
    return this;
};

function globalFunction() {
    expect(this).toBe(globalObject);

    expect(globalArrow()).toBe(globalThisValue);

    const arrowInGlobalFunction = () => this;
    expect(arrowInGlobalFunction()).toBe(globalObject);

    return arrowInGlobalFunction;
}

expect(globalArrow()).toBe(globalThisValue);
expect(globalFunction()()).toBe(globalObject);

const arrowFromGlobalFunction = globalFunction();

const customThisValue = {
    isCustomThis: true,
    variant: 0,
};

const otherCustomThisValue = {
    isCustomThis: true,
    variant: 1,
};

describe("describe with arrow function", () => {
    expect(this).toBe(globalThisValue);

    test("nested test with normal function should get global object", function () {
        expect(this).toBe(globalObject);
    });

    test("nested test with arrow function should get same this value as enclosing function", () => {
        expect(this).toBe(globalThisValue);
    });
});

describe("describe with normal function", function () {
    expect(this).toBe(globalObject);
    test("nested test with normal function should get global object", function () {
        expect(this).toBe(globalObject);
    });

    test("nested test with arrow function should get same this value as enclosing function", () => {
        expect(this).toBe(globalObject);
    });
});

describe("basic behavior", () => {
    expect(this).toBe(globalThisValue);

    expect(arrowFromGlobalFunction()).toBe(globalObject);

    expect(customThisValue).not.toBe(otherCustomThisValue);

    test("binding arrow function does not influence this value", () => {
        const boundGlobalArrow = globalArrow.bind({ shouldNotBeHere: true });

        expect(boundGlobalArrow()).toBe(globalThisValue);
    });

    function functionInArrow() {
        expect(arrowFromGlobalFunction()).toBe(globalObject);
        return this;
    }

    function functionWithArrow() {
        expect(arrowFromGlobalFunction()).toBe(globalObject);
        return () => {
            expect(arrowFromGlobalFunction()).toBe(globalObject);
            return this;
        };
    }

    function strictFunction() {
        "use strict";
        return this;
    }

    test("functions get globalObject as this value", () => {
        expect(functionInArrow()).toBe(globalObject);
        expect(functionWithArrow()()).toBe(globalObject);
    });

    test("strict functions get undefined as this value", () => {
        expect(strictFunction()).toBeUndefined();
    });

    test("bound function gets overwritten this value", () => {
        const boundFunction = functionInArrow.bind(customThisValue);
        expect(boundFunction()).toBe(customThisValue);

        const boundFunctionWithArrow = functionWithArrow.bind(customThisValue);
        expect(boundFunctionWithArrow()()).toBe(customThisValue);

        // However we cannot bind the arrow function itself
        const failingArrowBound = boundFunctionWithArrow().bind(otherCustomThisValue);
        expect(failingArrowBound()).toBe(customThisValue);

        const boundStrictFunction = strictFunction.bind(customThisValue);
        expect(boundStrictFunction()).toBe(customThisValue);
    });
});

describe("functions on created objects", () => {
    const obj = {
        func: function () {
            expect(arrowFromGlobalFunction()).toBe(globalObject);
            return this;
        },

        funcWithArrow: function () {
            expect(arrowFromGlobalFunction()).toBe(globalObject);
            return () => this;
        },

        arrow: () => {
            expect(arrowFromGlobalFunction()).toBe(globalObject);
            return this;
        },
        otherProperty: "yes",
    };

    test("function get this value of associated object", () => {
        expect(obj.func()).toBe(obj);
    });

    test("arrow function on object get above this value", () => {
        expect(obj.arrow()).toBe(globalThisValue);
    });

    test("arrow function from normal function from object has object as this value", () => {
        expect(obj.funcWithArrow()()).toBe(obj);
    });

    test("bound overwrites value of normal object function", () => {
        const boundFunction = obj.func.bind(customThisValue);
        expect(boundFunction()).toBe(customThisValue);

        const boundFunctionWithArrow = obj.funcWithArrow.bind(customThisValue);
        expect(boundFunctionWithArrow()()).toBe(customThisValue);

        const boundArrowFunction = obj.arrow.bind(customThisValue);
        expect(boundArrowFunction()).toBe(globalThisValue);
    });

    test("also works for object defined in function", () => {
        (function () {
            expect(arrowFromGlobalFunction()).toBe(globalObject);

            // It is bound below
            expect(this).toBe(customThisValue);

            const obj2 = {
                func: function () {
                    expect(arrowFromGlobalFunction()).toBe(globalObject);
                    return this;
                },

                arrow: () => {
                    expect(arrowFromGlobalFunction()).toBe(globalObject);
                    return this;
                },
                otherProperty: "also",
            };

            expect(obj2.func()).toBe(obj2);
            expect(obj2.arrow()).toBe(customThisValue);
        }).bind(customThisValue)();
    });
});

describe("behavior with classes", () => {
    class Basic {
        constructor(value) {
            expect(this).toBeInstanceOf(Basic);
            this.arrowFunctionInClass = () => {
                return this;
            };

            this.value = value;

            expect(arrowFromGlobalFunction()).toBe(globalObject);
        }

        func() {
            expect(arrowFromGlobalFunction()).toBe(globalObject);
            return this;
        }
    }

    const basic = new Basic(14);
    const basic2 = new Basic(457);

    expect(basic).not.toBe(basic2);

    test("calling functions on class should give instance as this value", () => {
        expect(basic.func()).toBe(basic);
        expect(basic2.func()).toBe(basic2);
    });

    test("calling arrow function created in constructor should give instance as this value", () => {
        expect(basic.arrowFunctionInClass()).toBe(basic);
        expect(basic2.arrowFunctionInClass()).toBe(basic2);
    });

    test("can bind function in class", () => {
        const boundFunction = basic.func.bind(customThisValue);
        expect(boundFunction()).toBe(customThisValue);

        const boundFunction2 = basic2.func.bind(otherCustomThisValue);
        expect(boundFunction2()).toBe(otherCustomThisValue);
    });
});

describe("derived classes behavior", () => {
    class Base {
        baseFunction() {
            expect(arrowFromGlobalFunction()).toBe(globalObject);

            return this;
        }
    }

    class Derived extends Base {
        constructor(value) {
            expect(arrowFromGlobalFunction()).toBe(globalObject);
            const arrowMadeBeforeSuper = () => {
                expect(this).toBeInstanceOf(Derived);
                return this;
            };
            super();
            expect(arrowMadeBeforeSuper()).toBe(this);

            this.arrowMadeBeforeSuper = arrowMadeBeforeSuper;
            this.arrowMadeAfterSuper = () => {
                expect(this).toBeInstanceOf(Derived);
                return this;
            };
            this.value = value;
        }

        derivedFunction() {
            expect(arrowFromGlobalFunction()).toBe(globalObject);
            return this;
        }
    }

    test("can create derived with arrow functions using this before super", () => {
        const testDerived = new Derived(-89);
        expect(testDerived.arrowMadeBeforeSuper()).toBe(testDerived);
        expect(testDerived.arrowMadeAfterSuper()).toBe(testDerived);
    });

    test("base and derived functions get correct this values", () => {
        const derived = new Derived(12);

        expect(derived.derivedFunction()).toBe(derived);
        expect(derived.baseFunction()).toBe(derived);
    });

    test("can bind derived and base functions", () => {
        const derived = new Derived(846);

        const boundDerivedFunction = derived.derivedFunction.bind(customThisValue);
        expect(boundDerivedFunction()).toBe(customThisValue);

        const boundBaseFunction = derived.baseFunction.bind(otherCustomThisValue);
        expect(boundBaseFunction()).toBe(otherCustomThisValue);
    });
});

describe("proxy behavior", () => {
    test("with no handler it makes no difference", () => {
        const globalArrowProxyNoHandler = new Proxy(globalArrow, {});
        expect(globalArrowProxyNoHandler()).toBe(globalThisValue);
    });

    test("proxy around global arrow still gives correct this value", () => {
        let lastThisArg = null;

        const handler = {
            apply(target, thisArg, argArray) {
                expect(target).toBe(globalArrow);
                lastThisArg = thisArg;
                expect(this).toBe(handler);

                return target(...argArray);
            },
        };

        const globalArrowProxy = new Proxy(globalArrow, handler);
        expect(globalArrowProxy()).toBe(globalThisValue);
        expect(lastThisArg).toBeUndefined();

        const boundProxy = globalArrowProxy.bind(customThisValue);
        expect(boundProxy()).toBe(globalThisValue);
        expect(lastThisArg).toBe(customThisValue);

        expect(globalArrowProxy.call(15)).toBe(globalThisValue);
        expect(lastThisArg).toBe(15);
    });
});

describe("derived classes which access this before super should fail", () => {
    class Base {}

    test("direct access of this should throw reference error", () => {
        class IncorrectConstructor extends Base {
            constructor() {
                this.something = "this will fail";
                super();
            }
        }

        expect(() => {
            new IncorrectConstructor();
        }).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("access of this via a arrow function", () => {
        class IncorrectConstructor extends Base {
            constructor() {
                const arrow = () => this;
                arrow();
                super();
            }
        }

        expect(() => {
            new IncorrectConstructor();
        }).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("access of this via a eval", () => {
        class IncorrectConstructor extends Base {
            constructor() {
                eval("this.foo = 'bar'");
                super();
            }
        }

        expect(() => {
            new IncorrectConstructor();
        }).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("access of this via a eval in arrow function", () => {
        class IncorrectConstructor extends Base {
            constructor() {
                const arrow = () => eval("() => this")();
                arrow();
                super();
            }
        }

        expect(() => {
            new IncorrectConstructor();
        }).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });

    test("access of this via arrow function even if bound with something else", () => {
        class IncorrectConstructor extends Base {
            constructor() {
                const arrow = () => this;
                const boundArrow = arrow.bind(customThisValue);
                boundArrow();
                super();
            }
        }

        expect(() => {
            new IncorrectConstructor();
        }).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
    });
});

describe("with statements", () => {
    test("this value is still the global object", () => {
        const obj = { haveValue: true, hello: "friends" };
        with (obj) {
            expect(this).toBe(globalThisValue);
            expect(hello).toBe("friends");
        }
    });

    test("with gets this value form outer scope", () => {
        const obj = { haveValue: true, hello: "friends" };

        function callme() {
            with (obj) {
                expect(this).toBe(customThisValue);
                expect(hello).toBe("friends");
            }
        }

        const boundMe = callme.bind(customThisValue);
        boundMe();
    });
});

describe("in non strict mode primitive this values are converted to objects", () => {
    const array = [true, false];

    // Technically the comma is implementation defined here. (Also for tests below.)
    expect(array.toLocaleString()).toBe("true,false");

    test("directly overwriting toString", () => {
        let count = 0;
        Boolean.prototype.toString = function () {
            count++;
            return typeof this;
        };

        expect(array.toLocaleString()).toBe("object,object");
        expect(count).toBe(2);
    });

    test("overwriting toString with a getter", () => {
        let count = 0;

        Object.defineProperty(Boolean.prototype, "toString", {
            get() {
                count++;
                const that = typeof this;
                return function () {
                    return that;
                };
            },
        });

        expect(array.toLocaleString()).toBe("object,object");
        expect(count).toBe(2);
    });
});
