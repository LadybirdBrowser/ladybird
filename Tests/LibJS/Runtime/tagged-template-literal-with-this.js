describe("tagged template literal this value in with statements", () => {
    test("this value should be the binding object when tag is resolved via with", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        with (obj) {
            tag`test`;
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is resolved via nested with", () => {
        let capturedThis;
        const obj1 = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };
        const obj2 = { obj: obj1 };

        with (obj2) {
            with (obj) {
                tag`test`;
            }
        }

        expect(capturedThis).toBe(obj1);
    });

    test("this value should be globalThis when tag is a local variable", () => {
        let capturedThis;
        function tag(strings) {
            capturedThis = this;
            return strings;
        }

        const obj = { foo: 42 };

        with (obj) {
            tag`test`;
        }

        expect(capturedThis).toBe(globalThis);
    });

    test("local identifier tag throws ReferenceError when in TDZ", () => {
        expect(() => {
            (function (a = tag`test`, tag = () => {}) {})();
        }).toThrowWithMessage(ReferenceError, "Binding <empty> is not initialized");
    });

    test("this value should be the binding object when tag is resolved via with with expressions", () => {
        let capturedThis;
        const obj = {
            tag(strings, ...values) {
                capturedThis = this;
                return strings;
            },
        };

        with (obj) {
            tag`test${42}`;
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is resolved via with with multiple expressions", () => {
        let capturedThis;
        const obj = {
            tag(strings, ...values) {
                capturedThis = this;
                return strings;
            },
        };

        with (obj) {
            tag`a${1}b${2}c`;
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is resolved via with in function", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        function test() {
            with (obj) {
                tag`test`;
            }
        }

        test();

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is resolved via with in arrow function", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        const test = () => {
            with (obj) {
                tag`test`;
            }
        };

        test();

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is resolved via with with proxy", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        const proxy = new Proxy(obj, {});

        with (proxy) {
            tag`test`;
        }

        expect(capturedThis).toBe(proxy);
    });

    test("this value should be the object when tag is a member expression inside with", () => {
        let capturedThis;
        const obj1 = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };
        const obj2 = { obj: obj1 };

        with (obj2) {
            obj.tag`test`;
        }

        expect(capturedThis).toBe(obj1);
    });

    test("this value should be the object when tag is a computed member expression inside with", () => {
        let capturedThis;
        const obj1 = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };
        const obj2 = { obj: obj1 };

        with (obj2) {
            obj["tag"]`test`;
        }

        expect(capturedThis).toBe(obj1);
    });

    test("this value should be the binding object when tag shadowing occurs", () => {
        let capturedThis;
        const obj1 = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };
        const obj2 = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        with (obj1) {
            with (obj2) {
                tag`test`;
            }
        }

        expect(capturedThis).toBe(obj2);
    });

    test("this value should be the binding object when tag is from with in try-catch", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        try {
            with (obj) {
                tag`test`;
            }
        } catch (e) {
            // Ignore
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in try-finally", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        try {
            with (obj) {
                tag`test`;
            }
        } finally {
            // Ignore
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in for loop", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        for (let i = 0; i < 1; i++) {
            with (obj) {
                tag`test`;
            }
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in while loop", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        let i = 0;
        while (i < 1) {
            with (obj) {
                tag`test`;
            }
            i++;
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in do-while loop", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        let i = 0;
        do {
            with (obj) {
                tag`test`;
            }
            i++;
        } while (i < 1);

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in if statement", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        if (true) {
            with (obj) {
                tag`test`;
            }
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in switch statement", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        switch (1) {
            case 1:
                with (obj) {
                    tag`test`;
                }
                break;
        }

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in eval", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        function tag(strings) {
            capturedThis = this;
            return strings;
        }

        eval("with (obj) { tag`test`; }");

        expect(capturedThis).toBe(obj);
    });

    test("this value should be the binding object when tag is from with in nested function", () => {
        let capturedThis;
        const obj = {
            tag(strings) {
                capturedThis = this;
                return strings;
            },
        };

        function outer() {
            function inner() {
                with (obj) {
                    tag`test`;
                }
            }
            inner();
        }

        outer();

        expect(capturedThis).toBe(obj);
    });

    test("this value tracks each with binding even when reused in a row", () => {
        const seen = [];
        const obj1 = {
            tag(strings) {
                seen.push(this);
                return strings;
            },
        };
        const obj2 = {
            tag(strings) {
                seen.push(this);
                return strings;
            },
        };

        with (obj1) {
            tag`first`;
        }
        with (obj2) {
            tag`second`;
        }

        expect(seen).toEqual([obj1, obj2]);
    });

    test("since getter returns functions per call the binding object still becomes this", () => {
        let capturedThis;
        const obj = {
            get tag() {
                return function (strings) {
                    capturedThis = this;
                    return strings;
                };
            },
        };

        with (obj) {
            tag`test`;
        }

        expect(capturedThis).toBe(obj);
    });
});
