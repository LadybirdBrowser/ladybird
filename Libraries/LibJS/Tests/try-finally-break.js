test("Nested try/catch/finally with break", () => {
    const executionOrder = [];

    try {
        for (const i = 1337; ; expect().fail("Jumped to for loop update block")) {
            try {
                try {
                    try {
                        break;
                    } finally {
                        expect(i).toBe(1337);
                        executionOrder.push(1);
                    }
                } finally {
                    expect(i).toBe(1337);
                    executionOrder.push(2);
                }
            } finally {
                expect(i).toBe(1337);
                executionOrder.push(3);
            }

            expect().fail("Running code after second to most outer try");
        }
    } finally {
        expect(() => {
            i;
        }).toThrowWithMessage(ReferenceError, "'i' is not defined");

        executionOrder.push(4);
    }

    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(executionOrder).toEqual([1, 2, 3, 4]);
});

test("Nested try/finally with labelled break", () => {
    const executionOrder = [];

    outer: try {
        const libjs = 0;
        expect(libjs).toBe(0);

        try {
            const serenity = 1;
            expect(libjs).toBe(0);
            expect(serenity).toBe(1);

            for (const i = 2; ; expect().fail("Jumped to for loop update block")) {
                const foo = 3;

                expect(libjs).toBe(0);
                expect(serenity).toBe(1);
                expect(i).toBe(2);
                expect(foo).toBe(3);

                try {
                    const bar = 4;

                    expect(libjs).toBe(0);
                    expect(serenity).toBe(1);
                    expect(i).toBe(2);
                    expect(foo).toBe(3);
                    expect(bar).toBe(4);

                    try {
                        const baz = 5;

                        expect(libjs).toBe(0);
                        expect(serenity).toBe(1);
                        expect(i).toBe(2);
                        expect(foo).toBe(3);
                        expect(bar).toBe(4);
                        expect(baz).toBe(5);

                        try {
                            const whf = 6;

                            expect(libjs).toBe(0);
                            expect(serenity).toBe(1);
                            expect(i).toBe(2);
                            expect(foo).toBe(3);
                            expect(bar).toBe(4);
                            expect(baz).toBe(5);
                            expect(whf).toBe(6);

                            break outer;
                        } finally {
                            const innerFinally1 = 7;

                            expect(() => {
                                whf;
                            }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                            expect(libjs).toBe(0);
                            expect(serenity).toBe(1);
                            expect(i).toBe(2);
                            expect(foo).toBe(3);
                            expect(bar).toBe(4);
                            expect(baz).toBe(5);
                            expect(innerFinally1).toBe(7);

                            executionOrder.push(1);
                        }

                        expect().fail("Running code after most inner try");
                    } finally {
                        const innerFinally2 = 8;

                        expect(() => {
                            baz;
                        }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

                        expect(() => {
                            whf;
                        }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                        expect(() => {
                            innerFinally1;
                        }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

                        expect(libjs).toBe(0);
                        expect(serenity).toBe(1);
                        expect(i).toBe(2);
                        expect(foo).toBe(3);
                        expect(bar).toBe(4);
                        expect(innerFinally2).toBe(8);

                        executionOrder.push(2);
                    }

                    expect().fail("Running code after second to most inner try");
                } finally {
                    const innerFinally3 = 9;

                    expect(() => {
                        bar;
                    }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

                    expect(() => {
                        baz;
                    }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

                    expect(() => {
                        whf;
                    }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                    expect(() => {
                        innerFinally1;
                    }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

                    expect(() => {
                        innerFinally2;
                    }).toThrowWithMessage(ReferenceError, "'innerFinally2' is not defined");

                    expect(libjs).toBe(0);
                    expect(serenity).toBe(1);
                    expect(i).toBe(2);
                    expect(foo).toBe(3);
                    expect(innerFinally3).toBe(9);

                    executionOrder.push(3);
                }

                expect().fail("Running code after third to most inner try");
            }

            expect().fail("Running code after for loop");
        } finally {
            const innerFinally4 = 10;

            expect(() => {
                serenity;
            }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

            expect(() => {
                i;
            }).toThrowWithMessage(ReferenceError, "'i' is not defined");

            expect(() => {
                foo;
            }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

            expect(() => {
                bar;
            }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

            expect(() => {
                baz;
            }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

            expect(() => {
                whf;
            }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

            expect(() => {
                innerFinally1;
            }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

            expect(() => {
                innerFinally2;
            }).toThrowWithMessage(ReferenceError, "'innerFinally2' is not defined");

            expect(() => {
                innerFinally3;
            }).toThrowWithMessage(ReferenceError, "'innerFinally3' is not defined");

            expect(libjs).toBe(0);
            expect(innerFinally4).toBe(10);

            executionOrder.push(4);
        }

        expect().fail("Running code after second to outer try");
    } finally {
        const innerFinally5 = 11;

        expect(() => {
            libjs;
        }).toThrowWithMessage(ReferenceError, "'libjs' is not defined");

        expect(() => {
            serenity;
        }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

        expect(() => {
            i;
        }).toThrowWithMessage(ReferenceError, "'i' is not defined");

        expect(() => {
            foo;
        }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

        expect(() => {
            bar;
        }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

        expect(() => {
            baz;
        }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

        expect(() => {
            whf;
        }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

        expect(() => {
            innerFinally1;
        }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

        expect(() => {
            innerFinally2;
        }).toThrowWithMessage(ReferenceError, "'innerFinally2' is not defined");

        expect(() => {
            innerFinally3;
        }).toThrowWithMessage(ReferenceError, "'innerFinally3' is not defined");

        expect(() => {
            innerFinally4;
        }).toThrowWithMessage(ReferenceError, "'innerFinally4' is not defined");

        expect(innerFinally5).toBe(11);

        executionOrder.push(5);
    }

    expect(() => {
        libjs;
    }).toThrowWithMessage(ReferenceError, "'libjs' is not defined");

    expect(() => {
        serenity;
    }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(() => {
        foo;
    }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

    expect(() => {
        bar;
    }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

    expect(() => {
        baz;
    }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

    expect(() => {
        whf;
    }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

    expect(() => {
        innerFinally1;
    }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

    expect(() => {
        innerFinally2;
    }).toThrowWithMessage(ReferenceError, "'innerFinally2' is not defined");

    expect(() => {
        innerFinally3;
    }).toThrowWithMessage(ReferenceError, "'innerFinally3' is not defined");

    expect(() => {
        innerFinally4;
    }).toThrowWithMessage(ReferenceError, "'innerFinally4' is not defined");

    expect(() => {
        innerFinally5;
    }).toThrowWithMessage(ReferenceError, "'innerFinally5' is not defined");

    expect(executionOrder).toEqual([1, 2, 3, 4, 5]);
});

test("labelled break in finally overrides labelled break in try", () => {
    const executionOrder = [];

    outer: for (const i = 1; ; expect().fail("Jumped to outer for loop update block")) {
        inner: for (const j = 2; ; expect().fail("Jumped to inner for loop update block")) {
            try {
                executionOrder.push(1);
                break inner;
            } finally {
                executionOrder.push(2);
                break outer;
            }

            expect().fail("Running code after try block");
        }

        expect().fail("Running code after inner for loop");
    }

    expect(executionOrder).toEqual([1, 2]);
});

test("Throw while breaking", () => {
    const executionOrder = [];
    expect(() => {
        try {
            for (const i = 1337; ; expect().fail("Jumped to for loop update block")) {
                try {
                    executionOrder.push(1);
                    break;
                } finally {
                    executionOrder.push(2);
                    throw Error(1);
                }
            }
        } finally {
            executionOrder.push(3);
        }
        expect().fail("Running code after for loop");
    }).toThrowWithMessage(Error, 1);

    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(executionOrder).toEqual([1, 2, 3]);
});

test("Throw while breaking with nested try-catch in finalizer", () => {
    const executionOrder = [];
    try {
        for (const i = 1337; ; expect().fail("Jumped to for loop update block")) {
            try {
                executionOrder.push(1);
                break;
            } finally {
                try {
                    throw 1;
                } catch {
                    executionOrder.push(2);
                }
                executionOrder.push(3);
            }
            expect().fail("Jumped out of inner finally statement");
        }
    } finally {
        executionOrder.push(4);
    }
    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(executionOrder).toEqual([1, 2, 3, 4]);
});

test("Throw while breaking with nested try-catch-finally in finalizer", () => {
    const executionOrder = [];
    try {
        for (const i = 1337; ; expect().fail("Jumped to for loop update block")) {
            try {
                executionOrder.push(1);
                break;
            } finally {
                try {
                    executionOrder.push(2);
                } catch {
                    expect.fail("Entered catch");
                } finally {
                    executionOrder.push(3);
                }
                executionOrder.push(4);
            }
            expect().fail("Jumped out of inner finally statement");
        }
    } finally {
        executionOrder.push(5);
    }
    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(executionOrder).toEqual([1, 2, 3, 4, 5]);
});

test("Throw while breaking with nested try-catch-finally with throw in finalizer", () => {
    const executionOrder = [];
    try {
        for (const i = 1337; ; expect().fail("Jumped to for loop update block")) {
            try {
                executionOrder.push(1);
                break;
            } finally {
                try {
                    throw 1;
                } catch {
                    executionOrder.push(2);
                } finally {
                    executionOrder.push(3);
                }
                executionOrder.push(4);
            }
            expect().fail("Jumped out of inner finally statement");
        }
    } finally {
        executionOrder.push(5);
    }
    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(executionOrder).toEqual([1, 2, 3, 4, 5]);
});

test("Labelled break with nested mixed try-catch/finally", () => {
    const executionOrder = [];
    scope: {
        try {
            try {
                executionOrder.push(1);
                break scope;
            } catch {
                expect.fail("Entered catch");
            }
            expect.fail("Continued after inner try");
        } finally {
            executionOrder.push(2);
        }
        expect.fail("Continued after outer try");
    }

    expect(executionOrder).toEqual([1, 2]);
});

test("Break with nested mixed try-catch/finally", () => {
    const executionOrder = [];
    do {
        try {
            try {
                executionOrder.push(1);
                break;
            } catch {
                expect.fail("Entered catch");
            }
            expect.fail("Continued after inner try");
        } finally {
            executionOrder.push(2);
        }
        expect.fail("Continued after outer try");
    } while (expect.fail("Continued after do-while loop"));

    expect(executionOrder).toEqual([1, 2]);
});
