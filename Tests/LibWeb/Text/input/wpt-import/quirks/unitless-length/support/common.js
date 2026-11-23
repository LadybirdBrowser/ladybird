setup({explicit_done:true});
onload = function() {
    setupIframe();

    var tests = [
    {input:"1", q:"1px"},
    {input:"+1", q:"1px"},
    {input:"-1", q:"-1px"},
    {input:"1.5", q:"1.5px"},
    {input:"+1.5", q:"1.5px"},
    {input:"-1.5", q:"-1.5px"},
    {input:"\\31 "},
    {input:"+\\31 "},
    {input:"-\\31 "},
    {input:"\\31 .5"},
    {input:"+\\31 .5"},
    {input:"-\\31 .5"},
    {input:"1\\31 "},
    {input:"+1\\31 "},
    {input:"-1\\31 "},
    {input:"1\\31 .5"},
    {input:"+1\\31 .5"},
    {input:"-1\\31 .5"},
    {input:"a"},
    {input:"A"},
    {input:"1a"},
    {input:"+1a"},
    {input:"-1a"},
    {input:"+1A"},
    {input:"-1A"},
    {input:"+a"},
    {input:"-a"},
    {input:"+A"},
    {input:"-A"},
    {input:"@a"},
    {input:"@1"},
    {input:"@1a"},
    {input:'"a"'},
    {input:'"1"'},
    {input:'"1a"'},
    {input:"url(1)"},
    {input:"url('1')"},
    {input:"#1"},
    {input:"#01"},
    {input:"#001"},
    {input:"#0001"},
    {input:"#00001"},
    {input:"#000001"},
    {input:"+/**/1"},
    {input:"-/**/1"},
    {input:"calc(1)"},
    {input:"calc(2 * 2px)", q:"4px", s:"4px"},
    {input:"1px 2", q:"1px 2px", shorthand:true},
    {input:"1 2px", q:"1px 2px", shorthand:true},
    {input:"1px calc(2)", shorthand:true},
    {input:"calc(1) 2px", shorthand:true},
    {input:"1 +2", q:"1px 2px", shorthand:true},
    {input:"1 -2", q:"1px -2px", shorthand:true},
    ];

    var props = [
    {prop:'background-position', check:'background-position', check_also:[]},
    {prop:'border-spacing', check:'border-spacing', check_also:[]},
    {prop:'border-top-width', check:'border-top-width'},
    {prop:'border-right-width', check:'border-right-width'},
    {prop:'border-bottom-width', check:'border-bottom-width'},
    {prop:'border-left-width', check:'border-left-width'},
    {prop:'border-width', check:'border-top-width', check_also:['border-right-width', 'border-bottom-width', 'border-left-width']},
    {prop:'bottom', check:'bottom'},
    {prop:'clip', check:'clip'},
    {prop:'font-size', check:'font-size'},
    {prop:'height', check:'height'},
    {prop:'left', check:'left'},
    {prop:'letter-spacing', check:'letter-spacing'},
    {prop:'margin-right', check:'margin-right'},
    {prop:'margin-left', check:'margin-left'},
    {prop:'margin-top', check:'margin-top'},
    {prop:'margin-bottom', check:'margin-bottom'},
    {prop:'margin', check:'margin-top', check_also:['margin-right', 'margin-bottom', 'margin-left']},
    {prop:'max-height', check:'max-height'},
    {prop:'max-width', check:'max-width'},
    {prop:'min-height', check:'min-height'},
    {prop:'min-width', check:'min-width'},
    {prop:'padding-top', check:'padding-top'},
    {prop:'padding-right', check:'padding-right'},
    {prop:'padding-bottom', check:'padding-bottom'},
    {prop:'padding-left', check:'padding-left'},
    {prop:'padding', check:'padding-top', check_also:['padding-right', 'padding-bottom', 'padding-left']},
    {prop:'right', check:'right'},
    {prop:'text-indent', check:'text-indent'},
    {prop:'top', check:'top'},
    {prop:'vertical-align', check:'vertical-align'},
    {prop:'width', check:'width'},
    {prop:'word-spacing', check:'word-spacing'},
    ];
    var style_template = '#test{border-style:solid;position:relative;{prop}:{test};}' +
                         '#ref{border-style:solid;position:relative;{prop}:{ref};}';

    tests.forEach(function(t) {
        for (var i in props) {
            if (t.shorthand && !(props[i].check_also)) {
                continue;
            }
            test(function() {
                win.style.textContent = style_template.replace('{test}', t.input)
                            .replace('{ref}', quirks ? t.q : t.s).replace(/\{prop\}/g, props[i].prop)
                            .replace(/clip:[^;]+/g, function(match) {
                                return 'clip:rect(auto, auto, auto, ' + match.substr(5) + ')';
                            });
                assert_equals(win.getComputedStyle(win.test).getPropertyValue(props[i].check),
                              win.getComputedStyle(win.ref).getPropertyValue(props[i].check),
                              props[i].prop);
                if (t.shorthand && props[i].check_also) {
                    for (var j in props[i].check_also) {
                        assert_equals(win.getComputedStyle(win.test).getPropertyValue(props[i].check_also[j]),
                                      win.getComputedStyle(win.ref).getPropertyValue(props[i].check_also[j]),
                                      props[i].prop + ', checking ' + props[i].check_also[j]);
                    }
                }
            }, props[i].prop + ": " + t.input);

        }
    });

    if (quirks) {
        var other_tests = [
        {input:'background:1 1', prop:'background-position'},
        {input:'border-top:red solid 1', prop:'border-top-width'},
        {input:'border-right:red solid 1', prop:'border-right-width'},
        {input:'border-bottom:red solid 1', prop:'border-bottom-width'},
        {input:'border-left:red solid 1', prop:'border-left-width'},
        {input:'border:red solid 1', prop:'border-top-width'},
        {input:'font:normal normal normal 40 sans-serif', prop:'font-size'},
        {input:'outline:red solid 1', prop:'outline-width'},
        {input:'outline-width:1', prop:'outline-width'},
        ];

        var other_template = "#test{position:relative;outline-style:solid;{test}}" +
                             "#ref{outline-style:solid}";

        other_tests.forEach(function(t) {
            test(function() {
                win.style.textContent = other_template.replace('{test}', t.input);
                assert_equals(win.getComputedStyle(win.test).getPropertyValue(t.prop),
                              win.getComputedStyle(win.ref).getPropertyValue(t.prop),
                              'quirk was supported');
            }, 'Excluded property '+t.input);
        });
    }

    done();
}
