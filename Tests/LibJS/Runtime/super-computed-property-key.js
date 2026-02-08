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
