test("reserved value word is not rejected when it appears in a nested object expression", () => {
    expect("(({a:{b:null}}))").toEval();
    expect("(({a:{b:true}}))").toEval();
    expect("(({a:{b:false}}))").toEval();
    expect("(({a:{b:{c:null}}}))").toEval();
    expect("(({a:{b:null},c:()=>0}))").toEval();
    expect("(({a:{b:true},c:()=>0}))").toEval();
    expect("(({a:{b:false},c:()=>0}))").toEval();
    expect("(({a:{b:{c:null}},c:()=>0}))").toEval();
    expect("(({a:{b:{c:this}},c:()=>0}))").toEval();
    expect("(({a:{b:{c:class{}}},c:()=>0}))").toEval();
    expect("(({a:{b:{c:function(){}}},c:()=>0}))").toEval();
});
