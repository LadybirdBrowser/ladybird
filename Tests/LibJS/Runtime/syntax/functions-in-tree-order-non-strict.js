// Note: This must be checked at script level because that is the only place
//       where function order is visible. We introduce some test(...) with
//       names to make sure the file does have some tests and a suite.

const evalViaArrow = x => eval(x);
const evalRef = eval;
const expectedBeforeEval = "1256MNQR3478CDHIOP";
const expectedAfterEval = "1256MNQR3478CDHIOPA9B";
const expectedAfterEvalRef = "1256MNQR3478CDHIOPA9BKJL";
const expectOrderToBe = expectedOrder => {
    const currentOrder = Object.getOwnPropertyNames(this)
        .filter(s => s.length === 2 && s[0] === "f")
        .map(s => s[1])
        .join("");
    expect(currentOrder).toBe(expectedOrder);
};

test("function order should be in tree order and nothing in eval should be included", () => {
    expectOrderToBe(expectedBeforeEval);
});

{
    function f1() {}

    expectOrderToBe(expectedBeforeEval);

    function f2() {}
}

expectOrderToBe(expectedBeforeEval);

function f3() {}

expectOrderToBe(expectedBeforeEval);

function f4() {}

expectOrderToBe(expectedBeforeEval);

{
    function f5() {}

    function f6() {}
}

function f7() {}

function f8() {}

expectOrderToBe(expectedBeforeEval);
eval(`
    expectOrderToBe(expectedAfterEval);
    
    function f9() {}

    {
        function fA() {}
    }
    
    function fB() {}

    expectOrderToBe(expectedAfterEval);
`);

expectOrderToBe(expectedAfterEval);

function fC() {}

function fD() {}

expectOrderToBe(expectedAfterEval);

// This eval does not do anything because it goes via a function, this means
// its parent environment is not the global environment so it does not have
// a global var environment and does not put the functions on `this`.
evalViaArrow(`
    expectOrderToBe(expectedAfterEval);

    function fE() {}

    {
        expectOrderToBe(expectedAfterEval);
        function fF() {}
    }

    function fG() {}
    
    expectOrderToBe(expectedAfterEval);
`);

test("function order should be in tree order, functions in eval should be in order but at the back", () => {
    expectOrderToBe(expectedAfterEval);
});

function fH() {}

function fI() {}

expectOrderToBe(expectedAfterEval);

// This is an indirect eval, but still has the global scope as immediate
// parent so it does influence the global `this`.
evalRef(`
    expectOrderToBe(expectedAfterEvalRef);
    console.log(2, JSON.stringify(Object.getOwnPropertyNames(this).filter(s => s.length === 2)));

    function fJ() {}

    {
        expectOrderToBe(expectedAfterEvalRef);
        function fK() {}
    }

    function fL() {}
    
    expectOrderToBe(expectedAfterEvalRef);
`);

{
    function fM() {}

    function fN() {}
}

test("function order should be in tree order, functions in evalRef should be in order but at the back", () => {
    expectOrderToBe(expectedAfterEvalRef);
});

function fO() {}

function fP() {}

{
    function fQ() {}

    {
        expectOrderToBe(expectedAfterEvalRef);
    }

    function fR() {}
}

expectOrderToBe(expectedAfterEvalRef);
