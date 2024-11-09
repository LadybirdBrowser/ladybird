test("Nested try/catch/finally with continue", () => {
    const executionOrder = [];

    function callFromUpdateBlock(i) {
        expect(i).toBe(2);
        executionOrder.push(4);
    }

    function callFromTestBlock(i) {
        expect(i).toBe(2);
        executionOrder.push(5);
        return false;
    }

    try {
        const foo = 0;

        expect(foo).toBe(0);

        for (let i = 1; i >= 2 ? callFromTestBlock(i) : true; ++i, callFromUpdateBlock(i)) {
            const bar = 2;

            expect(foo).toBe(0);
            expect(i).toBe(1);
            expect(bar).toBe(2);

            try {
                const baz = 3;

                expect(foo).toBe(0);
                expect(i).toBe(1);
                expect(bar).toBe(2);
                expect(baz).toBe(3);

                try {
                    const serenity = 4;

                    expect(foo).toBe(0);
                    expect(i).toBe(1);
                    expect(bar).toBe(2);
                    expect(baz).toBe(3);
                    expect(serenity).toBe(4);

                    try {
                        const whf = 5;

                        expect(foo).toBe(0);
                        expect(i).toBe(1);
                        expect(bar).toBe(2);
                        expect(baz).toBe(3);
                        expect(serenity).toBe(4);
                        expect(whf).toBe(5);

                        continue;
                    } finally {
                        const innerFinally1 = 6;

                        expect(() => {
                            whf;
                        }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                        expect(foo).toBe(0);
                        expect(i).toBe(1);
                        expect(bar).toBe(2);
                        expect(baz).toBe(3);
                        expect(serenity).toBe(4);
                        expect(innerFinally1).toBe(6);

                        executionOrder.push(1);
                    }

                    expect().fail("Running code after most inner try in for loop");
                } finally {
                    const innerFinally2 = 7;

                    expect(() => {
                        serenity;
                    }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

                    expect(() => {
                        whf;
                    }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                    expect(() => {
                        innerFinally1;
                    }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

                    expect(foo).toBe(0);
                    expect(i).toBe(1);
                    expect(bar).toBe(2);
                    expect(baz).toBe(3);
                    expect(innerFinally2).toBe(7);

                    executionOrder.push(2);
                }

                expect().fail("Running code from after the middle try in for loop");
            } finally {
                const innerFinally3 = 8;

                expect(() => {
                    baz;
                }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

                expect(() => {
                    serenity;
                }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

                expect(() => {
                    whf;
                }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                expect(() => {
                    innerFinally1;
                }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

                expect(() => {
                    innerFinally2;
                }).toThrowWithMessage(ReferenceError, "'innerFinally2' is not defined");

                expect(foo).toBe(0);
                expect(i).toBe(1);
                expect(bar).toBe(2);
                expect(innerFinally3).toBe(8);

                executionOrder.push(3);
            }

            expect().fail("Running code from after the outer try in for loop");
        }

        executionOrder.push(6);

        expect(() => {
            i;
        }).toThrowWithMessage(ReferenceError, "'i' is not defined");

        expect(() => {
            bar;
        }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

        expect(() => {
            baz;
        }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

        expect(() => {
            serenity;
        }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

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

        expect(foo).toBe(0);
    } finally {
        const innerFinally4 = 9;

        expect(() => {
            foo;
        }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

        expect(() => {
            i;
        }).toThrowWithMessage(ReferenceError, "'i' is not defined");

        expect(() => {
            bar;
        }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

        expect(() => {
            baz;
        }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

        expect(() => {
            serenity;
        }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

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

        expect(innerFinally4).toBe(9);

        executionOrder.push(7);
    }

    expect(() => {
        foo;
    }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(() => {
        bar;
    }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

    expect(() => {
        baz;
    }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

    expect(() => {
        serenity;
    }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

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

    expect(executionOrder).toEqual([1, 2, 3, 4, 5, 6, 7]);
});

test("Nested try/catch/finally with labelled continue", () => {
    const executionOrder = [];

    function callFromUpdateBlock(j) {
        expect(j).toBe(2);
        executionOrder.push(5);
    }

    function callFromTestBlock(j) {
        expect(j).toBe(2);
        executionOrder.push(6);
        return false;
    }

    try {
        const foo = 0;

        expect(foo).toBe(0);

        outer: for (let j = 1; j >= 2 ? callFromTestBlock(j) : true; ++j, callFromUpdateBlock(j)) {
            const bar = 2;

            expect(foo).toBe(0);
            expect(j).toBe(1);
            expect(bar).toBe(2);

            try {
                const baz = 3;

                expect(foo).toBe(0);
                expect(j).toBe(1);
                expect(bar).toBe(2);
                expect(baz).toBe(3);

                for (const i = 4; ; expect().fail("Jumped to inner for loop update block")) {
                    const serenity = 5;

                    expect(foo).toBe(0);
                    expect(j).toBe(1);
                    expect(bar).toBe(2);
                    expect(baz).toBe(3);
                    expect(i).toBe(4);
                    expect(serenity).toBe(5);

                    try {
                        const whf = 6;

                        expect(foo).toBe(0);
                        expect(j).toBe(1);
                        expect(bar).toBe(2);
                        expect(baz).toBe(3);
                        expect(i).toBe(4);
                        expect(serenity).toBe(5);
                        expect(whf).toBe(6);

                        try {
                            const beforeContinueTry = 7;

                            expect(foo).toBe(0);
                            expect(j).toBe(1);
                            expect(bar).toBe(2);
                            expect(baz).toBe(3);
                            expect(i).toBe(4);
                            expect(serenity).toBe(5);
                            expect(whf).toBe(6);
                            expect(beforeContinueTry).toBe(7);

                            try {
                                const continueTry = 8;

                                expect(foo).toBe(0);
                                expect(j).toBe(1);
                                expect(bar).toBe(2);
                                expect(baz).toBe(3);
                                expect(i).toBe(4);
                                expect(serenity).toBe(5);
                                expect(whf).toBe(6);
                                expect(beforeContinueTry).toBe(7);
                                expect(continueTry).toBe(8);

                                continue outer;
                            } finally {
                                const innerFinally1 = 9;

                                expect(() => {
                                    continueTry;
                                }).toThrowWithMessage(
                                    ReferenceError,
                                    "'continueTry' is not defined"
                                );

                                expect(foo).toBe(0);
                                expect(j).toBe(1);
                                expect(bar).toBe(2);
                                expect(baz).toBe(3);
                                expect(i).toBe(4);
                                expect(serenity).toBe(5);
                                expect(whf).toBe(6);
                                expect(beforeContinueTry).toBe(7);
                                expect(innerFinally1).toBe(9);

                                executionOrder.push(1);
                            }

                            expect().fail("Running code after most inner try");
                        } finally {
                            const innerFinally2 = 10;

                            expect(() => {
                                beforeContinueTry;
                            }).toThrowWithMessage(
                                ReferenceError,
                                "'beforeContinueTry' is not defined"
                            );

                            expect(() => {
                                continueTry;
                            }).toThrowWithMessage(ReferenceError, "'continueTry' is not defined");

                            expect(() => {
                                innerFinally1;
                            }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

                            expect(foo).toBe(0);
                            expect(j).toBe(1);
                            expect(bar).toBe(2);
                            expect(baz).toBe(3);
                            expect(i).toBe(4);
                            expect(serenity).toBe(5);
                            expect(whf).toBe(6);
                            expect(innerFinally2).toBe(10);

                            executionOrder.push(2);
                        }

                        expect().fail("Running code after second to most inner try");
                    } finally {
                        const innerFinally3 = 11;

                        expect(() => {
                            whf;
                        }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                        expect(() => {
                            beforeContinueTry;
                        }).toThrowWithMessage(ReferenceError, "'beforeContinueTry' is not defined");

                        expect(() => {
                            continueTry;
                        }).toThrowWithMessage(ReferenceError, "'continueTry' is not defined");

                        expect(() => {
                            innerFinally1;
                        }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

                        expect(() => {
                            innerFinally2;
                        }).toThrowWithMessage(ReferenceError, "'innerFinally2' is not defined");

                        expect(foo).toBe(0);
                        expect(j).toBe(1);
                        expect(bar).toBe(2);
                        expect(baz).toBe(3);
                        expect(i).toBe(4);
                        expect(serenity).toBe(5);
                        expect(innerFinally3).toBe(11);

                        executionOrder.push(3);
                    }

                    expect().fail("Running code after third to most inner try");
                }

                expect().fail("Running code after inner for loop");
            } finally {
                const innerFinally4 = 12;

                expect(() => {
                    baz;
                }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

                expect(() => {
                    i;
                }).toThrowWithMessage(ReferenceError, "'i' is not defined");

                expect(() => {
                    serenity;
                }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

                expect(() => {
                    whf;
                }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

                expect(() => {
                    beforeContinueTry;
                }).toThrowWithMessage(ReferenceError, "'beforeContinueTry' is not defined");

                expect(() => {
                    continueTry;
                }).toThrowWithMessage(ReferenceError, "'continueTry' is not defined");

                expect(() => {
                    innerFinally1;
                }).toThrowWithMessage(ReferenceError, "'innerFinally1' is not defined");

                expect(() => {
                    innerFinally2;
                }).toThrowWithMessage(ReferenceError, "'innerFinally2' is not defined");

                expect(() => {
                    innerFinally3;
                }).toThrowWithMessage(ReferenceError, "'innerFinally3' is not defined");

                expect(foo).toBe(0);
                expect(j).toBe(1);
                expect(bar).toBe(2);
                expect(innerFinally4).toBe(12);

                executionOrder.push(4);
            }

            expect().fail("Running code after try in outer for loop");
        }

        expect(() => {
            j;
        }).toThrowWithMessage(ReferenceError, "'j' is not defined");

        expect(() => {
            bar;
        }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

        expect(() => {
            baz;
        }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

        expect(() => {
            i;
        }).toThrowWithMessage(ReferenceError, "'i' is not defined");

        expect(() => {
            serenity;
        }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

        expect(() => {
            whf;
        }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

        expect(() => {
            beforeContinueTry;
        }).toThrowWithMessage(ReferenceError, "'beforeContinueTry' is not defined");

        expect(() => {
            continueTry;
        }).toThrowWithMessage(ReferenceError, "'continueTry' is not defined");

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

        expect(foo).toBe(0);

        executionOrder.push(7);
    } finally {
        const innerFinally5 = 13;

        expect(() => {
            foo;
        }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

        expect(() => {
            j;
        }).toThrowWithMessage(ReferenceError, "'j' is not defined");

        expect(() => {
            bar;
        }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

        expect(() => {
            baz;
        }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

        expect(() => {
            i;
        }).toThrowWithMessage(ReferenceError, "'i' is not defined");

        expect(() => {
            serenity;
        }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

        expect(() => {
            whf;
        }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

        expect(() => {
            beforeContinueTry;
        }).toThrowWithMessage(ReferenceError, "'beforeContinueTry' is not defined");

        expect(() => {
            continueTry;
        }).toThrowWithMessage(ReferenceError, "'continueTry' is not defined");

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

        expect(innerFinally5).toBe(13);

        executionOrder.push(8);
    }

    expect(() => {
        foo;
    }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

    expect(() => {
        j;
    }).toThrowWithMessage(ReferenceError, "'j' is not defined");

    expect(() => {
        bar;
    }).toThrowWithMessage(ReferenceError, "'bar' is not defined");

    expect(() => {
        baz;
    }).toThrowWithMessage(ReferenceError, "'baz' is not defined");

    expect(() => {
        i;
    }).toThrowWithMessage(ReferenceError, "'i' is not defined");

    expect(() => {
        serenity;
    }).toThrowWithMessage(ReferenceError, "'serenity' is not defined");

    expect(() => {
        whf;
    }).toThrowWithMessage(ReferenceError, "'whf' is not defined");

    expect(() => {
        beforeContinueTry;
    }).toThrowWithMessage(ReferenceError, "'beforeContinueTry' is not defined");

    expect(() => {
        continueTry;
    }).toThrowWithMessage(ReferenceError, "'continueTry' is not defined");

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

    expect(executionOrder).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
});

test("labelled continue in finally overrides labelled continue in try", () => {
    const executionOrder = [];

    function callFromUpdateBlock(i) {
        expect(i).toBe(2);
        executionOrder.push(3);
    }

    function callFromTestBlock(i) {
        expect(i).toBe(2);
        executionOrder.push(4);
        return false;
    }

    outer: for (let i = 1; i >= 2 ? callFromTestBlock(i) : true; ++i, callFromUpdateBlock(i)) {
        inner: for (const j = 2; ; expect().fail("Jumped to inner for loop update block")) {
            try {
                executionOrder.push(1);
                continue inner;
            } finally {
                executionOrder.push(2);
                continue outer;
            }

            expect().fail("Running code after try block");
        }

        expect().fail("Running code after inner for loop");
    }

    expect(executionOrder).toEqual([1, 2, 3, 4]);
});
