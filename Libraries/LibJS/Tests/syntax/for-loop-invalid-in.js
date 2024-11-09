test("The use of the 'in' keyword is restricted in for loop headers", () => {
    expect("for (a ? a : a in a; false; ) ;").not.toEval();
    expect("for (a + a in b);").not.toEval();
    expect("for (a in a of a);").not.toEval();
    expect("for(a = a in a; false;);").not.toEval();
    expect("for(a += a in a; false;);").not.toEval();
    expect("for(a -= a in a; false;);").not.toEval();
    expect("for(a *= a in a; false;);").not.toEval();
    expect("for(a /= a in a; false;);").not.toEval();
    expect("for(a &= a in a; false;);").not.toEval();
    expect("for(a |= a in a; false;);").not.toEval();
    expect("for(a ^= a in a; false;);").not.toEval();
    expect("for(a ^^= a in a; false;);").not.toEval();
    expect("for(a == a in a; false;);").not.toEval();
    expect("for(a != a in a; false;);").not.toEval();
    expect("for(a === a in a; false;);").not.toEval();
    expect("for(a !== a in a; false;);").not.toEval();
    expect("for (a ?? b in c;;);").not.toEval();
});
