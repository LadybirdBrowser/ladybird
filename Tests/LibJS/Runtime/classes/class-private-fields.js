test("basic functionality", () => {
    class A {
        #number = 3;

        getNumber() {
            return this.#number;
        }

        #string = "foo";

        getString() {
            return this.#string;
        }

        #uninitialized;

        getUninitialized() {
            return this.#uninitialized;
        }
    }

    const a = new A();
    expect(a.getNumber()).toBe(3);
    expect(a.getString()).toBe("foo");
    expect(a.getUninitialized()).toBeUndefined();

    expect("a.#number").not.toEval();
    expect("a.#string").not.toEval();
    expect("a.#uninitialized").not.toEval();
});

test("initializer has correct this value", () => {
    class A {
        #thisVal = this;

        getThisVal() {
            return this.#thisVal;
        }

        #thisName = this.#thisVal;

        getThisName() {
            return this.#thisName;
        }
    }

    const a = new A();
    expect(a.getThisVal()).toBe(a);
    expect(a.getThisName()).toBe(a);
});

test("static fields", () => {
    class A {
        static #simple = 1;

        static getStaticSimple() {
            return this.#simple;
        }

        static #thisVal = this;
        static #thisName = this.name;
        static #thisVal2 = this.#thisVal;

        static getThisVal() {
            return this.#thisVal;
        }

        static getThisName() {
            return this.#thisName;
        }

        static getThisVal2() {
            return this.#thisVal2;
        }
    }

    expect(A.getStaticSimple()).toBe(1);

    expect(A.getThisVal()).toBe(A);
    expect(A.getThisName()).toBe("A");
    expect(A.getThisVal2()).toBe(A);

    expect("A.#simple").not.toEval();
});

test("slash after private identifier is treated as division", () => {
    class A {
        static #field = 4;
        static #divided = this.#field / 2;

        static getDivided() {
            return this.#divided;
        }
    }

    expect(A.getDivided()).toBe(2);
});

test("private identifier not followed by 'in' throws", () => {
    expect(`class A { #field = 2; method() { return #field instanceof 1; }}`).not.toEval();
    expect(`class A { #field = 2; method() { return #field < 1; }}`).not.toEval();
    expect(`class A { #field = 2; method() { return #field + 1; }}`).not.toEval();
    expect(`class A { #field = 2; method() { return #field ** 1; }}`).not.toEval();
    expect(`class A { #field = 2; method() { return !#field; } }`).not.toEval();
    expect(`class A { #field = 2; method() { return ~#field; } }`).not.toEval();
    expect(`class A { #field = 2; method() { return ++#field; } }`).not.toEval();

    expect(`class A { #field = 2; method() { return #field in 1; }}`).toEval();
});

test("private identifier followed by 'in' with invalid grammar throws", () => {
    expect(`
class C {
  #field;

  constructor() {
    #field in () => {};
  }
}
    `).not.toEval();

    expect(`
class C {
  #field;

  constructor() {
    #field in #field in this;
  }
}
    `).not.toEval();

    expect(`
class C {
  #field;

  *gen() {
    #field in yield;
  }
}
    `).not.toEval();

    expect(`
class C {
  #field;

  *gen() {
    #field in yield this;
  }
}
    `).not.toEval();
});

test("private identifier followed by 'in' with parenthesized yield is valid", () => {
    class C {
        #field = 42;

        *gen(obj) {
            return #field in (yield obj);
        }
    }

    const c = new C();
    const it = c.gen({ "#field": true });
    it.next();
    const result = it.next(c);
    expect(result.value).toBe(true);
    expect(result.done).toBe(true);
});

test("cannot have static and non static field with the same description", () => {
    expect("class A { static #simple; #simple; }").not.toEval();
});

test("'arguments' is not allowed in class field initializer", () => {
    expect("class A { #a = arguments; }").not.toEval();
    expect("class B { static #b = arguments; }").not.toEval();

    class C {
        #c = eval("arguments");
    }

    expect(() => {
        new C();
    }).toThrowWithMessage(SyntaxError, "'arguments' is not allowed in class field initializer");

    expect(() => {
        class D {
            static #d = eval("arguments");
        }
    }).toThrowWithMessage(SyntaxError, "'arguments' is not allowed in class field initializer");
});

test("using 'arguments' via indirect eval throws at runtime instead of parse time", () => {
    const indirect = eval;

    class A {
        #a = indirect("arguments");
    }

    expect(() => {
        new A();
    }).toThrowWithMessage(ReferenceError, "'arguments' is not defined");

    expect(() => {
        class B {
            static #b = indirect("arguments");
        }
    }).toThrowWithMessage(ReferenceError, "'arguments' is not defined");
});

test("private names are visible to direct eval", () => {
    class A {
        #field = 1;

        #method() {
            return 2;
        }

        get #accessor() {
            return 3;
        }

        static #staticField = 4;

        initializer = eval("this.#field + this.#method() + this.#accessor");

        getWithEval() {
            return eval("this.#field + this.#method() + this.#accessor");
        }

        static getStaticWithEval() {
            return eval("this.#staticField");
        }
    }

    class B {
        #field = 10;

        #method() {
            return 20;
        }

        get #accessor() {
            return 30;
        }
    }

    const a = new A();
    expect(a.initializer).toBe(6);
    expect(a.getWithEval()).toBe(6);
    expect(A.getStaticWithEval()).toBe(4);
    expect(() => {
        a.getWithEval.call(new B());
    }).toThrow(TypeError);
});

test("unknown private names in direct eval throw during eval declaration instantiation", () => {
    class A {
        getWithEval() {
            return eval("this.#missing");
        }
    }

    expect(() => {
        new A().getWithEval();
    }).toThrowWithMessage(SyntaxError, "Reference to undeclared private field or method '#missing'");
});

test("unknown private name gives SyntaxError", () => {
    expect(`#n`).not.toEval();
    expect(`obj.#n`).not.toEval();
    expect(`this.#n`).not.toEval();
    expect(`if (#n) 1;`).not.toEval();
    expect(`1?.#n`).not.toEval();
    expect(`1?.n.#n`).not.toEval();
});

// OSS-FUZZ Issue 53363: top level unknown private names seg faults
expect(() => eval(`#n`)).toThrowWithMessage(SyntaxError, "Private identifier must be followed by 'in'");
expect(() => eval(`obj.#n`)).toThrowWithMessage(SyntaxError, "Reference to undeclared private field or method '#n'");
expect(() => eval(`this.#n`)).toThrowWithMessage(SyntaxError, "Reference to undeclared private field or method '#n'");
expect(() => eval(`if (#n) 1;`)).toThrowWithMessage(SyntaxError, "Private identifier must be followed by 'in'");
expect(() => eval(`1?.#n`)).toThrowWithMessage(SyntaxError, "Reference to undeclared private field or method '#n'");
expect(() => eval(`1?.n.#n`)).toThrowWithMessage(SyntaxError, "Reference to undeclared private field or method '#n'");
