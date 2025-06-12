const noHoistLexTopLevel = false;
let canCallNonHoisted = 0;

expect(basicHoistTopLevel()).toEqual("basicHoistTopLevel");

function basicHoistTopLevel() {
    return "basicHoistTopLevel";
}

expect(typeof noHoistLexTopLevel).toBe("boolean");
expect(typeof hoistInBlockTopLevel).toBe("undefined");

{
    expect(noHoistLexTopLevel()).toEqual("noHoistLexTopLevel");
    ++canCallNonHoisted;

    expect(basicHoistTopLevel()).toEqual("basicHoistTopLevelInBlock");
    ++canCallNonHoisted;

    expect(hoistInBlockTopLevel()).toEqual("hoistInBlockTopLevel");

    function hoistInBlockTopLevel() {
        return "hoistInBlockTopLevel";
    }

    function noHoistLexTopLevel() {
        return "noHoistLexTopLevel";
    }

    function basicHoistTopLevel() {
        return "basicHoistTopLevelInBlock";
    }
}

expect(canCallNonHoisted).toBe(2);

expect(hoistInBlockTopLevel()).toEqual("hoistInBlockTopLevel");

{
    {
        expect(nestedBlocksTopLevel()).toEqual("nestedBlocksTopLevel");

        function nestedBlocksTopLevel() {
            return "nestedBlocksTopLevel";
        }
    }
    expect(nestedBlocksTopLevel()).toEqual("nestedBlocksTopLevel");
}
expect(nestedBlocksTopLevel()).toEqual("nestedBlocksTopLevel");
expect(hoistInBlockTopLevel()).toEqual("hoistInBlockTopLevel");

expect(typeof hoistSecondOneTopLevel).toBe("undefined");
{
    expect(typeof hoistSecondOneTopLevel).toBe("undefined");

    {
        expect(hoistSecondOneTopLevel()).toEqual("hoistSecondOneTopLevel");

        function hoistSecondOneTopLevel() {
            return "hoistFirstOneTopLevel";
        }

        expect(hoistSecondOneTopLevel()).toEqual("hoistSecondOneTopLevel");

        function hoistSecondOneTopLevel() {
            return "hoistSecondOneTopLevel";
        }

        expect(hoistSecondOneTopLevel()).toEqual("hoistSecondOneTopLevel");

        {
            expect(hoistSecondOneTopLevel()).toEqual("hoistThirdOneTopLevel");

            function hoistSecondOneTopLevel() {
                return "hoistThirdOneTopLevel";
            }

            expect(hoistSecondOneTopLevel()).toEqual("hoistThirdOneTopLevel");
        }

        expect(hoistSecondOneTopLevel()).toEqual("hoistSecondOneTopLevel");
    }

    expect(hoistSecondOneTopLevel()).toEqual("hoistSecondOneTopLevel");
}

expect(hoistSecondOneTopLevel()).toEqual("hoistSecondOneTopLevel");

test("Non-strict function does hoist", () => {
    const noHoistLexFunction = false;
    let canCallNonHoisted = 0;

    expect(basicHoistFunction()).toEqual("basicHoistFunction");

    function basicHoistFunction() {
        return "basicHoistFunction";
    }

    expect(typeof noHoistLexFunction).toBe("boolean");
    expect(typeof hoistInBlockFunction).toBe("undefined");

    {
        expect(noHoistLexFunction()).toEqual("noHoistLexFunction");
        ++canCallNonHoisted;

        expect(basicHoistFunction()).toEqual("basicHoistFunctionInBlock");
        ++canCallNonHoisted;

        expect(hoistInBlockFunction()).toEqual("hoistInBlockFunction");

        function hoistInBlockFunction() {
            return "hoistInBlockFunction";
        }

        function noHoistLexFunction() {
            return "noHoistLexFunction";
        }

        function basicHoistFunction() {
            return "basicHoistFunctionInBlock";
        }
    }

    expect(canCallNonHoisted).toBe(2);

    expect(hoistInBlockFunction()).toEqual("hoistInBlockFunction");

    {
        {
            expect(nestedBlocksFunction()).toEqual("nestedBlocksFunction");

            function nestedBlocksFunction() {
                return "nestedBlocksFunction";
            }
        }
        expect(nestedBlocksFunction()).toEqual("nestedBlocksFunction");
    }
    expect(nestedBlocksFunction()).toEqual("nestedBlocksFunction");
    expect(hoistInBlockFunction()).toEqual("hoistInBlockFunction");

    expect(typeof hoistSecondOneFunction).toBe("undefined");
    {
        expect(typeof hoistSecondOneFunction).toBe("undefined");

        {
            expect(hoistSecondOneFunction()).toEqual("hoistSecondOneFunction");

            function hoistSecondOneFunction() {
                return "hoistFirstOneFunction";
            }

            expect(hoistSecondOneFunction()).toEqual("hoistSecondOneFunction");

            function hoistSecondOneFunction() {
                return "hoistSecondOneFunction";
            }

            expect(hoistSecondOneFunction()).toEqual("hoistSecondOneFunction");

            {
                expect(hoistSecondOneFunction()).toEqual("hoistThirdOneFunction");

                function hoistSecondOneFunction() {
                    return "hoistThirdOneFunction";
                }

                expect(hoistSecondOneFunction()).toEqual("hoistThirdOneFunction");
            }

            expect(hoistSecondOneFunction()).toEqual("hoistSecondOneFunction");
        }

        expect(hoistSecondOneFunction()).toEqual("hoistSecondOneFunction");
    }

    expect(hoistSecondOneFunction()).toEqual("hoistSecondOneFunction");

    expect(notBlockFunctionTopLevel()).toBe("second");

    function notBlockFunctionTopLevel() {
        return "first";
    }

    expect(notBlockFunctionTopLevel()).toBe("second");

    function notBlockFunctionTopLevel() {
        return "second";
    }

    expect(notBlockFunctionTopLevel()).toBe("second");
});

test("Strict function does not hoist", () => {
    "use strict";

    const noHoistLexStrictFunction = false;
    let canCallNonHoisted = 0;

    expect(basicHoistStrictFunction()).toEqual("basicHoistStrictFunction");

    function basicHoistStrictFunction() {
        return "basicHoistStrictFunction";
    }

    expect(typeof noHoistLexStrictFunction).toBe("boolean");
    // We cannot use expect(() => ).toThrow because that introduces extra scoping
    try {
        hoistInBlockStrictFunction;
        expect().fail();
    } catch (e) {
        expect(e).toBeInstanceOf(ReferenceError);
        expect(e.message).toEqual("'hoistInBlockStrictFunction' is not defined");
    }

    {
        expect(noHoistLexStrictFunction()).toEqual("noHoistLexStrictFunction");
        ++canCallNonHoisted;

        expect(basicHoistStrictFunction()).toEqual("basicHoistStrictFunctionInBlock");
        ++canCallNonHoisted;

        expect(hoistInBlockStrictFunction()).toEqual("hoistInBlockStrictFunction");

        function hoistInBlockStrictFunction() {
            return "hoistInBlockStrictFunction";
        }

        function noHoistLexStrictFunction() {
            return "noHoistLexStrictFunction";
        }

        function basicHoistStrictFunction() {
            return "basicHoistStrictFunctionInBlock";
        }
    }

    expect(canCallNonHoisted).toBe(2);

    try {
        hoistInBlockStrictFunction;
        expect().fail();
    } catch (e) {
        expect(e).toBeInstanceOf(ReferenceError);
        expect(e.message).toEqual("'hoistInBlockStrictFunction' is not defined");
    }

    {
        try {
            nestedBlocksStrictFunction;
            expect().fail();
        } catch (e) {
            expect(e).toBeInstanceOf(ReferenceError);
            expect(e.message).toEqual("'nestedBlocksStrictFunction' is not defined");
        }

        {
            expect(nestedBlocksStrictFunction()).toEqual("nestedBlocksStrictFunction");

            function nestedBlocksStrictFunction() {
                return "nestedBlocksStrictFunction";
            }
        }
        try {
            nestedBlocksStrictFunction;
            expect().fail();
        } catch (e) {
            expect(e).toBeInstanceOf(ReferenceError);
            expect(e.message).toEqual("'nestedBlocksStrictFunction' is not defined");
        }
    }

    try {
        nestedBlocksStrictFunction;
        expect().fail();
    } catch (e) {
        expect(e).toBeInstanceOf(ReferenceError);
        expect(e.message).toEqual("'nestedBlocksStrictFunction' is not defined");
    }

    expect(notBlockStrictFunctionTopLevel()).toBe("second");

    function notBlockStrictFunctionTopLevel() {
        return "first";
    }

    expect(notBlockStrictFunctionTopLevel()).toBe("second");

    function notBlockStrictFunctionTopLevel() {
        return "second";
    }

    {
        expect(notBlockStrictFunctionTopLevel()).toBe("third");

        function notBlockStrictFunctionTopLevel() {
            return "third";
        }

        expect(notBlockStrictFunctionTopLevel()).toBe("third");
    }

    expect(notBlockStrictFunctionTopLevel()).toBe("second");

    // Inside a block inside a strict function gives a syntax error
    let didNotRunEval = true;
    expect(`
        didNotRunEval = false;
        () => {
            "use strict";

            {
                function f() {
                    return "first";
                }

                function f() {
                    return "second";
                }
            }
        };
    `).not.toEval();

    expect(didNotRunEval).toBeTrue();

    // However, in eval it's fine but the function does not escape the eval
    {
        let ranEval = false;
        eval(`
            expect(hoistSecondOneStrictFunction()).toBe("hoistSecondOneStrictFunction");

            function hoistSecondOneStrictFunction() {
                return "hoistFirstOneStrictFunction";
            }

            function hoistSecondOneStrictFunction() {
                return "hoistSecondOneStrictFunction";
            }

            ranEval = true;
            `);

        expect(ranEval).toBeTrue();

        try {
            hoistSecondOneStrictFunction;
            expect().fail();
        } catch (e) {
            expect(e).toBeInstanceOf(ReferenceError);
            expect(e.message).toEqual("'hoistSecondOneStrictFunction' is not defined");
        }
    }
});
