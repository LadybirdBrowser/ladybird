test("super property with computed key using Symbol.toPrimitive", () => {
    let callOrder = [];

    let keyObj = {
        [Symbol.toPrimitive]() {
            callOrder.push("toPrimitive");
            return "testProp";
        },
    };

    class Parent {
        constructor() {
            Parent.prototype.testProp = "parent-value";
        }
    }

    class Child extends Parent {
        testMethod() {
            callOrder.push("before-access");
            const result = super[keyObj];
            callOrder.push("after-access");
            return result;
        }
    }

    const child = new Child();
    const result = child.testMethod();

    expect(result).toBe("parent-value");
    expect(callOrder).toEqual(["before-access", "toPrimitive", "after-access"]);
});

test("super property assignment with computed key using Symbol.toPrimitive", () => {
    let callOrder = [];

    let keyObj = {
        [Symbol.toPrimitive]() {
            callOrder.push("toPrimitive");
            return "testProp";
        },
    };

    function getRhsValue() {
        callOrder.push("rhs-evaluation");
        return "new-value";
    }

    class Parent {
        constructor() {
            Parent.prototype.testProp = "original-value";
        }
    }

    class Child extends Parent {
        testMethod() {
            callOrder.push("before-assignment");
            super[keyObj] = getRhsValue();
            callOrder.push("after-assignment");
            return this.testProp;
        }
    }

    const child = new Child();
    const result = child.testMethod();

    expect(result).toBe("new-value");
    // For assignment super[b] = c, ToPropertyKey should happen after c evaluation.
    expect(callOrder).toEqual(["before-assignment", "rhs-evaluation", "toPrimitive", "after-assignment"]);
});

test("super property with computed key that throws in Symbol.toPrimitive", () => {
    let callOrder = [];

    let keyObj = {
        [Symbol.toPrimitive]() {
            callOrder.push("toPrimitive-throws");
            throw new Error("ToPropertyKey error");
        },
    };

    class Parent {
        constructor() {
            Parent.prototype.testProp = "parent-value";
        }
    }

    class Child extends Parent {
        testMethod() {
            callOrder.push("before-access");
            try {
                const result = super[keyObj];
                callOrder.push("should-not-reach");
                return result;
            } catch (e) {
                callOrder.push("caught-exception");
                return "error-value";
            }
        }
    }

    const child = new Child();
    const result = child.testMethod();

    expect(result).toBe("error-value");
    expect(callOrder).toEqual(["before-access", "toPrimitive-throws", "caught-exception"]);
});

test("super property assignment with computed key that throws in Symbol.toPrimitive", () => {
    let callOrder = [];

    let keyObj = {
        [Symbol.toPrimitive]() {
            callOrder.push("toPrimitive-throws");
            throw new Error("ToPropertyKey error");
        },
    };

    class Parent {
        constructor() {
            Parent.prototype.testProp = "original-value";
        }
    }

    class Child extends Parent {
        testMethod() {
            callOrder.push("before-assignment");
            try {
                super[keyObj] = "new-value";
                callOrder.push("should-not-reach");
                return "success";
            } catch (e) {
                callOrder.push("caught-exception");
                return "error-value";
            }
        }
    }

    const child = new Child();
    const result = child.testMethod();

    expect(result).toBe("error-value");
    expect(callOrder).toEqual(["before-assignment", "toPrimitive-throws", "caught-exception"]);
});

test("super property with computed key using valueOf", () => {
    let callOrder = [];

    let keyObj = {
        toString() {
            callOrder.push("toString");
            return {};
        },
        valueOf() {
            callOrder.push("valueOf");
            return "testProp";
        },
    };

    class Parent {
        constructor() {
            Parent.prototype.testProp = "parent-value";
        }
    }

    class Child extends Parent {
        testMethod() {
            callOrder.push("before-access");
            const result = super[keyObj];
            callOrder.push("after-access");
            return result;
        }
    }

    const child = new Child();
    const result = child.testMethod();

    expect(result).toBe("parent-value");
    expect(callOrder).toEqual(["before-access", "toString", "valueOf", "after-access"]);
});

test("super property with computed key using toString", () => {
    let callOrder = [];

    let keyObj = {
        toString() {
            callOrder.push("toString");
            return "testProp";
        },
    };

    class Parent {
        constructor() {
            Parent.prototype.testProp = "parent-value";
        }
    }

    class Child extends Parent {
        testMethod() {
            callOrder.push("before-access");
            const result = super[keyObj];
            callOrder.push("after-access");
            return result;
        }
    }

    const child = new Child();
    const result = child.testMethod();

    expect(result).toBe("parent-value");
    expect(callOrder).toEqual(["before-access", "toString", "after-access"]);
});

test("super property with computed key returning Symbol", () => {
    let callOrder = [];
    let testSymbol = Symbol("test");

    let keyObj = {
        [Symbol.toPrimitive]() {
            callOrder.push("toPrimitive");
            return testSymbol;
        },
    };

    class Parent {
        constructor() {
            Parent.prototype[testSymbol] = "symbol-value";
        }
    }

    class Child extends Parent {
        testMethod() {
            callOrder.push("before-access");
            const result = super[keyObj];
            callOrder.push("after-access");
            return result;
        }
    }

    const child = new Child();
    const result = child.testMethod();

    expect(result).toBe("symbol-value");
    expect(callOrder).toEqual(["before-access", "toPrimitive", "after-access"]);
});

test("super property base is resolved after the computed key is evaluated", () => {
    function basesUsed(makeHome) {
        const log = [];
        const first = {
            set x(value) {
                log.push("first");
            },
            get x() {
                log.push("first");
                return () => {};
            },
        };
        const second = {
            set x(value) {
                log.push("second");
            },
            get x() {
                log.push("second");
                return () => {};
            },
        };
        const home = makeHome(() => Object.setPrototypeOf(home, second));
        Object.setPrototypeOf(home, first);
        home.method();
        return log;
    }

    expect(
        basesUsed(changePrototype => ({
            method() {
                [super[(changePrototype(), "x")]] = [1];
            },
        }))
    ).toEqual(["second"]);
    expect(
        basesUsed(changePrototype => ({
            method() {
                for (super[(changePrototype(), "x")] of [1]);
            },
        }))
    ).toEqual(["second"]);
    expect(
        basesUsed(changePrototype => ({
            method() {
                super[(changePrototype(), "x")]?.();
            },
        }))
    ).toEqual(["second"]);
});

test("super property assignment keeps a computed key the right-hand side reassigns", () => {
    const log = [];
    const parent = {
        set first(value) {
            log.push(`first = ${value}`);
        },
        set second(value) {
            log.push(`second = ${value}`);
        },
    };
    const home = {
        method() {
            let key = "first";
            super[key] = ((key = "second"), 1);
        },
    };
    Object.setPrototypeOf(home, parent);
    home.method();

    expect(log).toEqual(["first = 1"]);
});
