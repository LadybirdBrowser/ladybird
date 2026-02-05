// For the original (development) tests, we want to make a bunch of changes to
// the page as it loads.  We don't want this for the conformance tests, so let
// them opt out.
if (typeof testsJsLibraryOnly == "undefined" || !testsJsLibraryOnly) {
    // Alert the reader of egregious Opera bug that will make the specced
    // implementation horribly buggy
    //@{
    (function() {
        var div = document.createElement("div");
        div.appendChild(document.createElement("br"));
        document.body.insertBefore(div, document.body.firstChild);
        var range = document.createRange();
        range.setStart(div, 1);
        div.insertBefore(document.createElement("p"), div.firstChild);
        if (range.startOffset > range.startContainer.childNodes.length) {
            var warningDiv = document.createElement("p");
            document.body.insertBefore(warningDiv, document.body.firstChild);
            warningDiv.style.fontWeight = "bold";
            warningDiv.style.fontSize = "2em";
            warningDiv.style.color = "red";
            warningDiv.innerHTML = 'Your browser suffers from an <a href="http://software.hixie.ch/utilities/js/live-dom-viewer/saved/1028">egregious bug</a> in range mutation that will give incorrect results for the spec columns in many cases.  To ensure that the spec column contains the output actually required by the spec, use a different browser.';
        }
        div.parentNode.removeChild(div);
    })();
    //@}

    // Insert the toolbar thingie as soon as the script file is loaded
    //@{
    (function() {
        var toolbarDiv = document.createElement("div");
        toolbarDiv.id = "toolbar";
        // Note: this is completely not a hack at all.
        toolbarDiv.innerHTML = "<style id=alerts>body > div > table > tbody > tr:not(.alert):not(:first-child):not(.active) { display: none }</style>"
            + "<label><input id=alert-checkbox type=checkbox accesskey=a checked onclick='updateAlertRowStyle()'> Display rows without spec <u>a</u>lerts</label>"
            + "<label><input id=browser-checkbox type=checkbox accesskey=b checked onclick='localStorage[\"display-browser-tests\"] = event.target.checked'> Run <u>b</u>rowser tests as well as spec tests</label>";

        document.body.appendChild(toolbarDiv);
    })();
    //@}

    // Confusingly, we're storing a string here, not a boolean.
    document.querySelector("#alert-checkbox").checked = localStorage["display-alerts"] != "false";
    document.querySelector("#browser-checkbox").checked = localStorage["display-browser-tests"] != "false";

    function updateAlertRowStyle() {
    //@{
        var checked = document.querySelector("#alert-checkbox").checked;
        document.querySelector("#alerts").disabled = checked;
        localStorage["display-alerts"] = checked;
    }
    //@}
    updateAlertRowStyle();

    // Feature-test whether the browser wraps at <wbr> or not, and set word-wrap:
    // break-word where necessary if not.  (IE and Opera don't wrap, Gecko and
    // WebKit do.)  word-wrap: break-word will break anywhere at all, so it looks
    // significantly uglier.
    //@{
    (function() {
        var wordWrapTestDiv = document.createElement("div");
        wordWrapTestDiv.style.width = "5em";
        document.body.appendChild(wordWrapTestDiv);
        wordWrapTestDiv.innerHTML = "abc";
        var height1 = getComputedStyle(wordWrapTestDiv).height;
        wordWrapTestDiv.innerHTML = "abc<wbr>abc<wbr>abc<wbr>abc<wbr>abc<wbr>abc";
        var height2 = getComputedStyle(wordWrapTestDiv).height;
        document.body.removeChild(wordWrapTestDiv);
        if (height1 == height2) {
            document.body.className = (document.body.className + " wbr-workaround").trim();
        }
    })();
    //@}
}

// Now for the meat of the file.
var tests = {
    backcolor: [
    //@{ Same as hilitecolor (set below)
    ],
    //@}
    bold: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        'foo<span contenteditable=false>[bar]</span>baz',
        'fo[o<span contenteditable=false>bar</span>b]az',
        'foo<span contenteditable=false>ba[r</span>b]az',
        'fo[o<span contenteditable=false>b]ar</span>baz',
        'fo[<b>o</b><span contenteditable=false>bar</span><b>b</b>]az',
        '<span contenteditable=false>foo<span contenteditable=true>[bar]</span>baz</span>',
        '<span contenteditable=false>fo[o<span contenteditable=true>bar</span>b]az</span>',
        '<span contenteditable=false>foo<span contenteditable=true>ba[r</span>b]az</span>',
        '<span contenteditable=false>fo[o<span contenteditable=true>b]ar</span>baz</span>',
        '<span contenteditable=false>fo[<b>o<span contenteditable=true>bar</span>b</b>]az</span>',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<span style="font-weight: bold">[bar]</span>baz',
        'foo<b>[bar]</b>baz',
        'foo<b>bar</b>[baz]',
        '[foo]<b>bar</b>baz',
        '<b>foo</b>[bar]<b>baz</b>',
        'foo<strong>bar</strong>[baz]',
        '[foo]<strong>bar</strong>baz',
        '<strong>foo</strong>[bar]<strong>baz</strong>',
        '<b>foo</b>[bar]<strong>baz</strong>',
        '<strong>foo</strong>[bar]<b>baz</b>',
        'foo[<b>bar</b>]baz',
        'foo[<b>bar]</b>baz',
        'foo<b>[bar</b>]baz',

        'foo{<b></b>}baz',
        'foo{<i></i>}baz',
        'foo{<b><i></i></b>}baz',
        'foo{<i><b></b></i>}baz',

        'foo<strong>[bar]</strong>baz',
        'foo[<strong>bar</strong>]baz',
        'foo[<strong>bar]</strong>baz',
        'foo<strong>[bar</strong>]baz',
        'foo[<span style="font-weight: bold">bar</span>]baz',
        'foo[<span style="font-weight: bold">bar]</span>baz',
        'foo<span style="font-weight: bold">[bar</span>]baz',

        '<b>{<p>foo</p><p>bar</p>}<p>baz</p></b>',
        '<b><p>foo[<i>bar</i>}</p><p>baz</p></b>',

        'foo [bar <b>baz] qoz</b> quz sic',
        'foo bar <b>baz [qoz</b> quz] sic',

        '<b id=purple>bar [baz] qoz</b>',

        'foo<span style="font-weight: 100">[bar]</span>baz',
        'foo<span style="font-weight: 200">[bar]</span>baz',
        'foo<span style="font-weight: 300">[bar]</span>baz',
        'foo<span style="font-weight: 400">[bar]</span>baz',
        'foo<span style="font-weight: 500">[bar]</span>baz',
        'foo<span style="font-weight: 600">[bar]</span>baz',
        'foo<span style="font-weight: 700">[bar]</span>baz',
        'foo<span style="font-weight: 800">[bar]</span>baz',
        'foo<span style="font-weight: 900">[bar]</span>baz',
        'foo<span style="font-weight: 400">[bar</span>]baz',
        'foo<span style="font-weight: 700">[bar</span>]baz',
        'foo[<span style="font-weight: 400">bar]</span>baz',
        'foo[<span style="font-weight: 700">bar]</span>baz',
        'foo[<span style="font-weight: 400">bar</span>]baz',
        'foo[<span style="font-weight: 700">bar</span>]baz',
        '<span style="font-weight: 100">foo[bar]baz</span>',
        '<span style="font-weight: 400">foo[bar]baz</span>',
        '<span style="font-weight: 700">foo[bar]baz</span>',
        '<span style="font-weight: 900">foo[bar]baz</span>',
        '{<span style="font-weight: 100">foobar]baz</span>',
        '{<span style="font-weight: 400">foobar]baz</span>',
        '{<span style="font-weight: 700">foobar]baz</span>',
        '{<span style="font-weight: 900">foobar]baz</span>',
        '<span style="font-weight: 100">foo[barbaz</span>}',
        '<span style="font-weight: 400">foo[barbaz</span>}',
        '<span style="font-weight: 700">foo[barbaz</span>}',
        '<span style="font-weight: 900">foo[barbaz</span>}',

        '<h3>foo[bar]baz</h3>',
        '{<h3>foobar]baz</h3>',
        '<h3>foo[barbaz</h3>}',
        '<h3>[foobarbaz]</h3>',
        '{<h3>foobarbaz]</h3>',
        '<h3>[foobarbaz</h3>}',
        '{<h3>foobarbaz</h3>}',

        '<b>foo<span style="font-weight: normal">bar<b>[baz]</b>quz</span>qoz</b>',
        '<b>foo<span style="font-weight: normal">[bar]</span>baz</b>',

        '{<b>foo</b> <b>bar</b>}',
        '{<h3>foo</h3><b>bar</b>}',

        '<i><b>foo</b></i>[bar]<i><b>baz</b></i>',
        '<i><b>foo</b></i>[bar]<b>baz</b>',
        '<b>foo</b>[bar]<i><b>baz</b></i>',
        '<font color=blue face=monospace><b>foo</b></font>[bar]',

        'foo<span style="font-weight: normal"><b>{bar}</b></span>baz',
        '[foo<span class=notbold>bar</span>baz]',
        '<b><span class=notbold>[foo]</span></b>',
        '<b><span class=notbold>foo[bar]baz</span></b>',

        '<p style="font-weight: bold">foo[bar]baz</p>',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<b>b]ar</b>baz',
        'foo<b>ba[r</b>b]az',
        'fo[o<b>bar</b>b]az',
        'foo[<b>b]ar</b>baz',
        'foo<b>ba[r</b>]baz',
        'foo{<b>bar</b>}baz',
        'fo[o<span style=font-weight:bold>b]ar</span>baz',
        '<span style=font-weight:800>fo[o</span><span style=font-weight:900>b]ar</span>',
        '<span style=font-weight:700>fo[o</span><span style=font-weight:800>b]ar</span>',
        '<span style=font-weight:600>fo[o</span><span style=font-weight:700>b]ar</span>',
        '<span style=font-weight:500>fo[o</span><span style=font-weight:600>b]ar</span>',
        '<span style=font-weight:400>fo[o</span><span style=font-weight:500>b]ar</span>',
        '<span style=font-weight:300>fo[o</span><span style=font-weight:400>b]ar</span>',
        '<span style=font-weight:200>fo[o</span><span style=font-weight:300>b]ar</span>',
        '<span style=font-weight:100>fo[o</span><span style=font-weight:200>b]ar</span>',
    ],
    //@}
    createlink: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        '<a href=http://www.google.com/>foo[bar]baz</a>',
        '<a href=http://www.google.com/>foo[barbaz</a>}',
        '{<a href=http://www.google.com/>foobar]baz</a>',
        '{<a href=http://www.google.com/>foobarbaz</a>}',
        '<a href=http://www.google.com/>[foobarbaz]</a>',

        'foo<a href=http://www.google.com/>[bar]</a>baz',
        '[foo]<a href=http://www.google.com/>bar</a>baz',
        'foo<a href=http://www.google.com/>bar</a>[baz]',
        'foo[<a href=http://www.google.com/>bar</a>]baz',
        'foo<a href=http://www.google.com/>[bar</a>baz]',
        '[foo<a href=http://www.google.com/>bar]</a>baz',
        '[foo<a href=http://www.google.com/>bar</a>baz]',

        '<a href=otherurl>foo[bar]baz</a>',
        '<a href=otherurl>foo[barbaz</a>}',
        '{<a href=otherurl>foobar]baz</a>',
        '{<a href=otherurl>foobarbaz</a>}',
        '<a href=otherurl>[foobarbaz]</a>',

        'foo<a href=otherurl>[bar]</a>baz',
        'foo[<a href=otherurl>bar</a>]baz',
        'foo<a href=otherurl>[bar</a>baz]',
        '[foo<a href=otherurl>bar]</a>baz',
        '[foo<a href=otherurl>bar</a>baz]',

        '<a href=otherurl><b>foo[bar]baz</b></a>',
        '<a href=otherurl><b>foo[barbaz</b></a>}',
        '{<a href=otherurl><b>foobar]baz</b></a>',
        '<a href=otherurl><b>[foobarbaz]</b></a>',

        '<a name=abc>foo[bar]baz</a>',
        '<a name=abc><b>foo[bar]baz</b></a>',

        ['', 'foo[bar]baz'],
    ],
    //@}
    // Opera requires this to be quoted, contrary to ES5 11.1.5 which allows
    // PropertyName to be any IdentifierName, and see 7.6 which defines
    // IdentifierName to include ReservedWord; Identifier excludes it.
    "delete": [
    //@{
        // Collapsed selection
        //
        // These three commented-out test call Firefox 5.0a2 to blow up, not
        // just throwing exceptions on the tests themselves but on many
        // subsequent tests too.
        //'[]foo',
        //'<span>[]foo</span>',
        //'<p>[]foo</p>',
        'foo[]bar',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo<span style=display:none>bar</span>[]baz',
        'foo<script>bar</script>[]baz',

        'fo&ouml;[]bar',
        'foo&#x308;[]bar',
        'foo&#x308;&#x327;[]bar',
        '&ouml;[]bar',
        'o&#x308;[]bar',
        'o&#x308;&#x327;[]bar',

        '&#x5e9;&#x5c1;&#x5b8;[]&#x5dc;&#x5d5;&#x5b9;&#x5dd;',
        '&#x5e9;&#x5c1;&#x5b8;&#x5dc;&#x5d5;&#x5b9;[]&#x5dd;',

        '<p>foo</p><p>[]bar</p>',
        '<p>foo</p>[]bar',
        'foo<p>[]bar</p>',
        '<p>foo<br></p><p>[]bar</p>',
        '<p>foo<br></p>[]bar',
        'foo<br><p>[]bar</p>',
        '<p>foo<br><br></p><p>[]bar</p>',
        '<p>foo<br><br></p>[]bar',
        'foo<br><br><p>[]bar</p>',

        '<div><p>foo</p></div><p>[]bar</p>',
        '<p>foo</p><div><p>[]bar</p></div>',
        '<div><p>foo</p></div><div><p>[]bar</p></div>',
        '<div><p>foo</p></div>[]bar',
        'foo<div><p>[]bar</p></div>',

        '<div>foo</div><div>[]bar</div>',
        '<pre>foo</pre>[]bar',

        'foo<br>[]bar',
        'foo<br><b>[]bar</b>',
        'foo<hr>[]bar',
        '<p>foo<hr><p>[]bar',
        '<p>foo</p><br><p>[]bar</p>',
        '<p>foo</p><br><br><p>[]bar</p>',
        '<p>foo</p><img src=/img/lion.svg><p>[]bar',
        'foo<img src=/img/lion.svg>[]bar',

        '<a>foo</a>[]bar',
        '<a href=/>foo</a>[]bar',
        '<a name=abc>foo</a>[]bar',
        '<a href=/ name=abc>foo</a>[]bar',
        '<span><a>foo</a></span>[]bar',
        '<span><a href=/>foo</a></span>[]bar',
        '<span><a name=abc>foo</a></span>[]bar',
        '<span><a href=/ name=abc>foo</a></span>[]bar',
        'foo<a>[]bar</a>',
        'foo<a href=/>[]bar</a>',
        'foo<a name=abc>[]bar</a>',
        'foo<a href=/ name=abc>[]bar</a>',

        'foo &nbsp;[]',
        '&nbsp;[] foo',
        'foo &nbsp;[]bar',
        'foo&nbsp; []bar',
        'foo&nbsp;&nbsp;[]bar',
        'foo  []bar',
        'foo []&nbsp; bar',
        'foo &nbsp;[] bar',
        'foo &nbsp; []bar',
        'foo []<span>&nbsp;</span> bar',
        'foo <span>&nbsp;</span>[] bar',
        'foo <span>&nbsp;</span> []bar',
        '<b>foo </b>&nbsp;[]bar',
        '<b>foo&nbsp;</b> []bar',
        '<b>foo&nbsp;</b>&nbsp;[]bar',
        '<b>foo </b> []bar',
        '<p>foo </p><p>[] bar</p>',

        '<pre>foo &nbsp;[]</pre>',
        '<pre>&nbsp;[] foo</pre>',
        '<pre>foo &nbsp;[]bar</pre>',
        '<pre>foo&nbsp; []bar</pre>',
        '<pre>foo  []bar</pre>',

        '<div style=white-space:pre>foo &nbsp;[]</div>',
        '<div style=white-space:pre>&nbsp;[] foo</div>',
        '<div style=white-space:pre>foo &nbsp;[]bar</div>',
        '<div style=white-space:pre>foo&nbsp; []bar</div>',
        '<div style=white-space:pre>foo  []bar</div>',

        '<div style=white-space:pre-wrap>foo &nbsp;[]</div>',
        '<div style=white-space:pre-wrap>&nbsp;[] foo</div>',
        '<div style=white-space:pre-wrap>foo &nbsp;[]bar</div>',
        '<div style=white-space:pre-wrap>foo&nbsp; []bar</div>',
        '<div style=white-space:pre-wrap>foo  []bar</div>',

        '<div style=white-space:pre-line>foo &nbsp;[]</div>',
        '<div style=white-space:pre-line>&nbsp;[] foo</div>',
        '<div style=white-space:pre-line>foo &nbsp;[]bar</div>',
        '<div style=white-space:pre-line>foo&nbsp; []bar</div>',
        '<div style=white-space:pre-line>foo  []bar</div>',

        '<div style=white-space:nowrap>foo &nbsp;[]</div>',
        '<div style=white-space:nowrap>&nbsp;[] foo</div>',
        '<div style=white-space:nowrap>foo &nbsp;[]bar</div>',
        '<div style=white-space:nowrap>foo&nbsp; []bar</div>',
        '<div style=white-space:nowrap>foo  []bar</div>',

        // Tables with collapsed selection
        'foo<table><tr><td>[]bar</table>baz',
        'foo<table><tr><td>bar</table>[]baz',
        '<p>foo<table><tr><td>[]bar</table><p>baz',
        '<p>foo<table><tr><td>bar</table><p>[]baz',
        '<table><tr><td>foo<td>[]bar</table>',
        '<table><tr><td>foo<tr><td>[]bar</table>',

        'foo<br><table><tr><td>[]bar</table>baz',
        'foo<table><tr><td>bar<br></table>[]baz',
        '<p>foo<br><table><tr><td>[]bar</table><p>baz',
        '<p>foo<table><tr><td>bar<br></table><p>[]baz',
        '<table><tr><td>foo<br><td>[]bar</table>',
        '<table><tr><td>foo<br><tr><td>[]bar</table>',

        'foo<br><br><table><tr><td>[]bar</table>baz',
        'foo<table><tr><td>bar<br><br></table>[]baz',
        '<p>foo<br><br><table><tr><td>[]bar</table><p>baz',
        '<p>foo<table><tr><td>bar<br><br></table><p>[]baz',
        '<table><tr><td>foo<br><br><td>[]bar</table>',
        '<table><tr><td>foo<br><br><tr><td>[]bar</table>',

        'foo<hr><table><tr><td>[]bar</table>baz',
        'foo<table><tr><td>bar<hr></table>[]baz',
        '<table><tr><td>foo<hr><td>[]bar</table>',
        '<table><tr><td>foo<hr><tr><td>[]bar</table>',

        // Lists with collapsed selection
        'foo<ol><li>[]bar<li>baz</ol>',
        'foo<br><ol><li>[]bar<li>baz</ol>',
        'foo<br><br><ol><li>[]bar<li>baz</ol>',
        '<ol><li>foo<li>[]bar</ol>',
        '<ol><li>foo<br><li>[]bar</ol>',
        '<ol><li>foo<br><br><li>[]bar</ol>',
        '<ol><li>foo<li>[]bar<br>baz</ol>',
        '<ol><li>foo<br>bar<li>[]baz</ol>',

        '<ol><li><p>foo</p>{}bar</ol>',

        '<ol><li><p>foo<li>[]bar</ol>',
        '<ol><li>foo<li><p>[]bar</ol>',
        '<ol><li><p>foo<li><p>[]bar</ol>',

        '<ol><li>foo<ul><li>[]bar</ul></ol>',
        'foo<ol><ol><li>[]bar</ol></ol>',
        'foo<div><ol><li>[]bar</ol></div>',

        'foo<dl><dt>[]bar<dd>baz</dl>',
        'foo<dl><dd>[]bar</dl>',
        '<dl><dt>foo<dd>[]bar</dl>',
        '<dl><dt>foo<dt>[]bar<dd>baz</dl>',
        '<dl><dt>foo<dd>bar<dd>[]baz</dl>',

        '<ol><li>foo</ol>[]bar',
        '<ol><li>foo<br></ol>[]bar',
        '<ol><li>foo<br><br></ol>[]bar',
        '<ol><li><br></ol>[]bar',
        '<ol><li>foo<li><br></ol>[]bar',

        '<ol><li>foo</ol><p>[]bar',
        '<ol><li>foo<br></ol><p>[]bar',
        '<ol><li>foo<br><br></ol><p>[]bar',
        '<ol><li><br></ol><p>[]bar',
        '<ol><li>foo<li><br></ol><p>[]bar',

        '<ol><li>foo</ol>{}<br>',
        '<ol><li>foo<br></ol>{}<br>',
        '<ol><li>foo<br><br></ol>{}<br>',
        '<ol><li><br></ol>{}<br>',
        '<ol><li>foo<li><br></ol>{}<br>',

        '<ol><li>foo</ol><p>{}<br>',
        '<ol><li>foo<br></ol><p>{}<br>',
        '<ol><li>foo<br><br></ol><p>{}<br>',
        '<ol><li><br></ol><p>{}<br>',
        '<ol><li>foo<li><br></ol><p>{}<br>',

        // Indented stuff with collapsed selection
        'foo<blockquote>[]bar</blockquote>',
        'foo<blockquote><blockquote>[]bar</blockquote></blockquote>',
        'foo<blockquote><div>[]bar</div></blockquote>',
        'foo<blockquote style="color: blue">[]bar</blockquote>',

        'foo<blockquote><blockquote><p>[]bar<p>baz</blockquote></blockquote>',
        'foo<blockquote><div><p>[]bar<p>baz</div></blockquote>',
        'foo<blockquote style="color: blue"><p>[]bar<p>baz</blockquote>',

        'foo<blockquote><p><b>[]bar</b><p>baz</blockquote>',
        'foo<blockquote><p><strong>[]bar</strong><p>baz</blockquote>',
        'foo<blockquote><p><span>[]bar</span><p>baz</blockquote>',

        'foo<blockquote><ol><li>[]bar</ol></blockquote><p>extra',
        'foo<blockquote>bar<ol><li>[]baz</ol>quz</blockquote><p>extra',
        'foo<blockquote><ol><li>bar</li><ol><li>[]baz</ol><li>quz</ol></blockquote><p>extra',

        // Invisible stuff with collapsed selection
        'foo<span></span>[]bar',
        'foo<span><span></span></span>[]bar',
        'foo<quasit></quasit>[]bar',
        'foo<br><span></span>[]bar',
        '<span>foo<span></span></span>[]bar',
        'foo<span></span><span>[]bar</span>',
        'foo<div><div><p>[]bar</div></div>',
        'foo<div><div><p><!--abc-->[]bar</div></div>',
        'foo<div><div><!--abc--><p>[]bar</div></div>',
        'foo<div><!--abc--><div><p>[]bar</div></div>',
        'foo<!--abc--><div><div><p>[]bar</div></div>',
        '<div><div><p>foo</div></div>[]bar',
        '<div><div><p>foo</div></div><!--abc-->[]bar',
        '<div><div><p>foo</div><!--abc--></div>[]bar',
        '<div><div><p>foo</p><!--abc--></div></div>[]bar',
        '<div><div><p>foo<!--abc--></div></div>[]bar',
        '<div><div><p>foo</p></div></div><div><div><div>[]bar</div></div></div>',
        '<div><div><p>foo<!--abc--></p></div></div><div><div><div>[]bar</div></div></div>',
        '<div><div><p>foo</p><!--abc--></div></div><div><div><div>[]bar</div></div></div>',
        '<div><div><p>foo</p></div><!--abc--></div><div><div><div>[]bar</div></div></div>',
        '<div><div><p>foo</p></div></div><!--abc--><div><div><div>[]bar</div></div></div>',
        '<div><div><p>foo</p></div></div><div><!--abc--><div><div>[]bar</div></div></div>',
        '<div><div><p>foo</p></div></div><div><div><!--abc--><div>[]bar</div></div></div>',
        '<div><div><p>foo</p></div></div><div><div><div><!--abc-->[]bar</div></div></div>',

        // Styled stuff with collapsed selection
        '<p style=color:blue>foo<p>[]bar',
        '<p style=color:blue>foo<p style=color:brown>[]bar',
        '<p style=color:blue>foo<p style=color:rgba(0,0,255,1)>[]bar',
        '<p style=color:transparent>foo<p style=color:rgba(0,0,0,0)>[]bar',
        '<p>foo<p style=color:brown>[]bar',
        '<p><font color=blue>foo</font><p>[]bar',
        '<p><font color=blue>foo</font><p><font color=brown>[]bar</font>',
        '<p>foo<p><font color=brown>[]bar</font>',
        '<p><span style=color:blue>foo</font><p>[]bar',
        '<p><span style=color:blue>foo</font><p><span style=color:brown>[]bar</font>',
        '<p>foo<p><span style=color:brown>[]bar</font>',

        '<p style=background-color:aqua>foo<p>[]bar',
        '<p style=background-color:aqua>foo<p style=background-color:tan>[]bar',
        '<p>foo<p style=background-color:tan>[]bar',
        '<p><span style=background-color:aqua>foo</font><p>[]bar',
        '<p><span style=background-color:aqua>foo</font><p><span style=background-color:tan>[]bar</font>',
        '<p>foo<p><span style=background-color:tan>[]bar</font>',

        '<p style=text-decoration:underline>foo<p>[]bar',
        '<p style=text-decoration:underline>foo<p style=text-decoration:line-through>[]bar',
        '<p>foo<p style=text-decoration:line-through>[]bar',
        '<p><u>foo</u><p>[]bar',
        '<p><u>foo</u><p><s>[]bar</s>',
        '<p>foo<p><s>[]bar</s>',

        '<p style=color:blue>foo</p>[]bar',
        'foo<p style=color:brown>[]bar',
        '<div style=color:blue><p style=color:green>foo</div>[]bar',
        '<div style=color:blue><p style=color:green>foo</div><p style=color:brown>[]bar',
        '<p style=color:blue>foo<div style=color:brown><p style=color:green>[]bar',

        // Uncollapsed selection
        'foo[bar]baz',
        '<p>foo<span style=color:#aBcDeF>[bar]</span>baz',
        '<p>foo<span style=color:#aBcDeF>{bar}</span>baz',
        '<p>foo{<span style=color:#aBcDeF>bar</span>}baz',
        '<p>[foo<span style=color:#aBcDeF>bar]</span>baz',
        '<p>{foo<span style=color:#aBcDeF>bar}</span>baz',
        '<p>foo<span style=color:#aBcDeF>[bar</span>baz]',
        '<p>foo<span style=color:#aBcDeF>{bar</span>baz}',
        '<p>foo<span style=color:#aBcDeF>[bar</span><span style=color:#fEdCbA>baz]</span>quz',

        'foo<b>[bar]</b>baz',
        'foo<b>{bar}</b>baz',
        'foo{<b>bar</b>}baz',
        'foo<span>[bar]</span>baz',
        'foo<span>{bar}</span>baz',
        'foo{<span>bar</span>}baz',
        '<b>foo[bar</b><i>baz]quz</i>',
        '<p>foo</p><p>[bar]</p><p>baz</p>',
        '<p>foo</p><p>{bar}</p><p>baz</p>',
        '<p>foo</p><p>{bar</p>}<p>baz</p>',
        '<p>foo</p>{<p>bar}</p><p>baz</p>',
        '<p>foo</p>{<p>bar</p>}<p>baz</p>',

        '<p>foo[bar<p>baz]quz',
        '<p>foo[bar<div>baz]quz</div>',
        '<p>foo[bar<h1>baz]quz</h1>',
        '<div>foo[bar</div><p>baz]quz',
        '<blockquote>foo[bar</blockquote><pre>baz]quz</pre>',

        '<p><b>foo[bar</b><p>baz]quz',
        '<div><p>foo[bar</div><p>baz]quz',
        '<p>foo[bar<blockquote><p>baz]quz<p>qoz</blockquote',
        '<p>foo[bar<p style=color:blue>baz]quz',
        '<p>foo[bar<p><b>baz]quz</b>',

        '<div><p>foo<p>[bar<p>baz]</div>',

        'foo[<br>]bar',
        '<p>foo[</p><p>]bar</p>',
        '<p>foo[</p><p>]bar<br>baz</p>',
        'foo[<p>]bar</p>',
        'foo{<p>}bar</p>',
        'foo[<p>]bar<br>baz</p>',
        'foo[<p>]bar</p>baz',
        'foo{<p>bar</p>}baz',
        'foo<p>{bar</p>}baz',
        'foo{<p>bar}</p>baz',
        '<p>foo[</p>]bar',
        '<p>foo{</p>}bar',
        '<p>foo[</p>]bar<br>baz',
        '<p>foo[</p>]bar<p>baz</p>',
        'foo[<div><p>]bar</div>',
        '<div><p>foo[</p></div>]bar',
        'foo[<div><p>]bar</p>baz</div>',
        'foo[<div>]bar<p>baz</p></div>',
        '<div><p>foo</p>bar[</div>]baz',
        '<div>foo<p>bar[</p></div>]baz',

        '<p>foo<br>{</p>]bar',
        '<p>foo<br><br>{</p>]bar',
        'foo<br>{<p>]bar</p>',
        'foo<br><br>{<p>]bar</p>',
        '<p>foo<br>{</p><p>}bar</p>',
        '<p>foo<br><br>{</p><p>}bar</p>',

        '<table><tbody><tr><th>foo<th>[bar]<th>baz<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>foo<th>ba[r<th>b]az<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>fo[o<th>bar<th>b]az<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>foo<th>bar<th>ba[z<tr><td>q]uz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>[foo<th>bar<th>baz]<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>[foo<th>bar<th>baz<tr><td>quz<td>qoz<td>qiz]</table>',
        '{<table><tbody><tr><th>foo<th>bar<th>baz<tr><td>quz<td>qoz<td>qiz</table>}',
        '<table><tbody><tr><td>foo<td>ba[r<tr><td>baz<td>quz<tr><td>q]oz<td>qiz</table>',
        '<p>fo[o<table><tr><td>b]ar</table><p>baz',
        '<p>foo<table><tr><td>ba[r</table><p>b]az',
        '<p>fo[o<table><tr><td>bar</table><p>b]az',

        '<p>foo<ol><li>ba[r<li>b]az</ol><p>quz',
        '<p>foo<ol><li>bar<li>[baz]</ol><p>quz',
        '<p>fo[o<ol><li>b]ar<li>baz</ol><p>quz',
        '<p>foo<ol><li>bar<li>ba[z</ol><p>q]uz',
        '<p>fo[o<ol><li>bar<li>b]az</ol><p>quz',
        '<p>fo[o<ol><li>bar<li>baz</ol><p>q]uz',

        '<ol><li>fo[o</ol><ol><li>b]ar</ol>',
        '<ol><li>fo[o</ol><ul><li>b]ar</ul>',

        'foo[<ol><li>]bar</ol>',
        '<ol><li>foo[<li>]bar</ol>',
        'foo[<dl><dt>]bar<dd>baz</dl>',
        'foo[<dl><dd>]bar</dl>',
        '<dl><dt>foo[<dd>]bar</dl>',
        '<dl><dt>foo[<dt>]bar<dd>baz</dl>',
        '<dl><dt>foo<dd>bar[<dd>]baz</dl>',

        '<b>foo [&nbsp;</b>bar]',
        'foo<b> [&nbsp;bar]</b>',
        '<b>[foo&nbsp;] </b>bar',
        '[foo<b>&nbsp;] bar</b>',

        // Do we merge based on element names or the display property?
        '<p style=display:inline>fo[o<p style=display:inline>b]ar',
        '<span style=display:block>fo[o</span><span style=display:block>b]ar</span>',
        '<span style=display:inline-block>fo[o</span><span style=display:inline-block>b]ar</span>',
        '<span style=display:inline-table>fo[o</span><span style=display:inline-table>b]ar</span>',
        '<span style=display:none>fo[o</span><span style=display:none>b]ar</span>',
        '<quasit style=display:block>fo[o</quasit><quasit style=display:block>b]ar</quasit>',

        // https://bugs.webkit.org/show_bug.cgi?id=35281
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=13976
        '<ol><li>foo</ol>{}<br><ol><li>bar</ol>',
        '<ol><li>foo</ol><p>{}<br></p><ol><li>bar</ol>',
        '<ol><li><p>foo</ol><p>{}<br></p><ol><li>bar</ol>',
        '<ol id=a><li>foo</ol>{}<br><ol><li>bar</ol>',
        '<ol><li>foo</ol>{}<br><ol id=b><li>bar</ol>',
        '<ol id=a><li>foo</ol>{}<br><ol id=b><li>bar</ol>',
        '<ol class=a><li>foo</ol>{}<br><ol class=b><li>bar</ol>',
        // Broken test: http://www.w3.org/Bugs/Public/show_bug.cgi?id=14727
        '!<ol><ol><li>foo</ol><li>{}<br><ol><li>bar</ol></ol>',
        '<ol><ol><li>foo</ol><li>{}<br></li><ol><li>bar</ol></ol>',
        '<ol><li>foo[</ol>bar]<ol><li>baz</ol>',
        '<ol><li>foo[</ol><p>bar]<ol><li>baz</ol>',
        '<ol><li><p>foo[</ol><p>bar]<ol><li>baz</ol>',
        '<ol><li>foo[]</ol><ol><li>bar</ol>',
        '<ol><li>foo</ol>[bar<ol><li>]baz</ol>',
        '<ol><li>foo</ol><p>[bar<ol><li>]baz</ol>',
        '<ol><li>foo</ol><p>[bar<ol><li><p>]baz</ol>',
        '<ol><li>foo</ol><ol><li>b[]ar</ol>',
        '<ol><ol><li>foo[</ol><li>bar</ol>baz]<ol><li>quz</ol>',
        '<ul><li>foo</ul>{}<br><ul><li>bar</ul>',
        '<ul><li>foo</ul><p>{}<br></p><ul><li>bar</ul>',
        '<ol><li>foo[<li>bar]</ol><ol><li>baz</ol><ol><li>quz</ol>',
        '<ol><li>foo</ol>{}<br><ul><li>bar</ul>',
        '<ol><li>foo</ol><p>{}<br></p><ul><li>bar</ul>',
        '<ul><li>foo</ul>{}<br><ol><li>bar</ol>',
        '<ul><li>foo</ul><p>{}<br></p><ol><li>bar</ol>',

        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=13831
        '<p><b>[foo]</b>',
        '<p><quasit>[foo]</quasit>',
        '<p><b><i>[foo]</i></b>',
        '<p><b>{foo}</b>',
        '<p>{<b>foo</b>}',
        '<p><b>f[]</b>',
        '<b>[foo]</b>',
        '<div><b>[foo]</b></div>',
    ],
    //@}
    fontname: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<code>[bar]</code>baz',
        'foo<kbd>[bar]</kbd>baz',
        'foo<listing>[bar]</listing>baz',
        'foo<pre>[bar]</pre>baz',
        'foo<samp>[bar]</samp>baz',
        'foo<tt>[bar]</tt>baz',

        'foo<code>b[a]r</code>baz',
        'foo<kbd>b[a]r</kbd>baz',
        'foo<listing>b[a]r</listing>baz',
        'foo<pre>b[a]r</pre>baz',
        'foo<samp>b[a]r</samp>baz',
        'foo<tt>b[a]r</tt>baz',

        '[foo<code>bar</code>baz]',
        '[foo<kbd>bar</kbd>baz]',
        '[foo<listing>bar</listing>baz]',
        '[foo<pre>bar</pre>baz]',
        '[foo<samp>bar</samp>baz]',
        '[foo<tt>bar</tt>baz]',

        '[foo<code>ba]r</code>baz',
        '[foo<kbd>ba]r</kbd>baz',
        '[foo<listing>ba]r</listing>baz',
        '[foo<pre>ba]r</pre>baz',
        '[foo<samp>ba]r</samp>baz',
        '[foo<tt>ba]r</tt>baz',

        'foo<code>b[ar</code>baz]',
        'foo<kbd>b[ar</kbd>baz]',
        'foo<listing>b[ar</listing>baz]',
        'foo<pre>b[ar</pre>baz]',
        'foo<samp>b[ar</samp>baz]',
        'foo<tt>b[ar</tt>baz]',

        'foo<span style="font-family: sans-serif">[bar]</span>baz',
        'foo<span style="font-family: sans-serif">b[a]r</span>baz',
        'foo<span style="font-family: monospace">[bar]</span>baz',
        'foo<span style="font-family: monospace">b[a]r</span>baz',

        'foo<tt contenteditable=false>ba[r</tt>b]az',
        'fo[o<tt contenteditable=false>b]ar</tt>baz',
        'foo<tt>{}<br></tt>bar',
        'foo<tt>{<br></tt>}bar',
        'foo<tt>{<br></tt>b]ar',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<span style=font-family:monospace>b]ar</span>baz',
        'foo<span style=font-family:monospace>ba[r</span>b]az',
        'fo[o<span style=font-family:monospace>bar</span>b]az',
        'foo[<span style=font-family:monospace>b]ar</span>baz',
        'foo<span style=font-family:monospace>ba[r</span>]baz',
        'foo[<span style=font-family:monospace>bar</span>]baz',
        'foo<span style=font-family:monospace>[bar]</span>baz',
        'foo{<span style=font-family:monospace>bar</span>}baz',
        'fo[o<code>b]ar</code>',
        'fo[o<kbd>b]ar</kbd>',
        'fo[o<listing>b]ar</listing>',
        'fo[o<pre>b]ar</pre>',
        'fo[o<samp>b]ar</samp>',
        'fo[o<tt>b]ar</tt>',
        '<tt>fo[o</tt><code>b]ar</code>',
        '<pre>fo[o</pre><samp>b]ar</samp>',
        '<span style=font-family:monospace>fo[o</span><kbd>b]ar</kbd>',
    ],
    //@}
    fontsize: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        ["1", 'foo[bar]baz'],
        ["0", 'foo[bar]baz'],
        ["-5", 'foo[bar]baz'],
        ["6", 'foo[bar]baz'],
        ["7", 'foo[bar]baz'],
        ["8", 'foo[bar]baz'],
        ["100", 'foo[bar]baz'],
        ["2em", 'foo[bar]baz'],
        ["20pt", 'foo[bar]baz'],
        ["xx-large", 'foo[bar]baz'],
        [" 1 ", 'foo[bar]baz'],
        ["1.", 'foo[bar]baz'],
        ["1.0", 'foo[bar]baz'],
        ["1.0e2", 'foo[bar]baz'],
        ["1.1", 'foo[bar]baz'],
        ["1.9", 'foo[bar]baz'],
        ["+0", 'foo[bar]baz'],
        ["+1", 'foo[bar]baz'],
        ["+9", 'foo[bar]baz'],
        ["-0", 'foo[bar]baz'],
        ["-1", 'foo[bar]baz'],
        ["-9", 'foo[bar]baz'],
        ["", 'foo[bar]baz'],

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<font size=1>[bar]</font>baz',
        '<font size=1>foo[bar]baz</font>',
        'foo<font size=3>[bar]</font>baz',
        '<font size=3>foo[bar]baz</font>',
        'foo<font size=4>[bar]</font>baz',
        '<font size=4>foo[bar]baz</font>',
        'foo<font size=+1>[bar]</font>baz',
        '<font size=+1>foo[bar]baz</font>',
        '<font size=4>foo<font size=1>b[a]r</font>baz</font>',

        'foo<span style="font-size: xx-small">[bar]</span>baz',
        '<span style="font-size: xx-small">foo[bar]baz</span>',
        'foo<span style="font-size: medium">[bar]</span>baz',
        '<span style="font-size: medium">foo[bar]baz</span>',
        'foo<span style="font-size: large">[bar]</span>baz',
        '<span style="font-size: large">foo[bar]baz</span>',
        '<span style="font-size: large">foo<span style="font-size: xx-small">b[a]r</span>baz</span>',

        'foo<span style="font-size: 2em">[bar]</span>baz',
        '<span style="font-size: 2em">foo[bar]baz</span>',

        '<p style="font-size: xx-small">foo[bar]baz</p>',
        '<p style="font-size: medium">foo[bar]baz</p>',
        '<p style="font-size: large">foo[bar]baz</p>',
        '<p style="font-size: 2em">foo[bar]baz</p>',

        ["3", '<p style="font-size: xx-small">foo[bar]baz</p>'],
        ["3", '<p style="font-size: medium">foo[bar]baz</p>'],
        ["3", '<p style="font-size: large">foo[bar]baz</p>'],
        ["3", '<p style="font-size: 2em">foo[bar]baz</p>'],

        // Minor algorithm bug: this changes the size of the "b" and "r" in
        // "bar" when we pull down styles
        ["3", '<font size=6>foo <span style="font-size: 2em">b[a]r</span> baz</font>'],

        ["3", 'foo<big>[bar]</big>baz'],
        ["3", 'foo<big>b[a]r</big>baz'],
        ["3", 'foo<small>[bar]</small>baz'],
        ["3", 'foo<small>b[a]r</small>baz'],

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<font size=2>b]ar</font>baz',
        'foo<font size=2>ba[r</font>b]az',
        'fo[o<font size=2>bar</font>b]az',
        'foo[<font size=2>b]ar</font>baz',
        'foo<font size=2>ba[r</font>]baz',
        'foo[<font size=2>bar</font>]baz',
        'foo<font size=2>[bar]</font>baz',
        'foo{<font size=2>bar</font>}baz',
        '<font size=1>fo[o</font><span style=font-size:xx-small>b]ar</span>',
        '<font size=2>fo[o</font><span style=font-size:small>b]ar</span>',
        '<font size=3>fo[o</font><span style=font-size:medium>b]ar</span>',
        '<font size=4>fo[o</font><span style=font-size:large>b]ar</span>',
        '<font size=5>fo[o</font><span style=font-size:x-large>b]ar</span>',
        '<font size=6>fo[o</font><span style=font-size:xx-large>b]ar</span>',

        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=13829
        ["!6", '<span style=background-color:aqua>[foo]</span>'],
        ["!6", '<span style=background-color:aqua>foo[bar]baz</span>'],
        ["!6", '[foo<span style=background-color:aqua>bar</span>baz]'],
    ],
    //@}
    forecolor: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        ['blue', 'foo[bar]baz'],
        ['f', 'foo[bar]baz'],
        ['#f', 'foo[bar]baz'],
        ['00f', 'foo[bar]baz'],
        ['#00f', 'foo[bar]baz'],
        ['0000ff', 'foo[bar]baz'],
        ['#0000ff', 'foo[bar]baz'],
        ['000000fff', 'foo[bar]baz'],
        ['#000000fff', 'foo[bar]baz'],
        ['rgb(0, 0, 255)', 'foo[bar]baz'],
        ['rgb(0%, 0%, 100%)', 'foo[bar]baz'],
        ['rgb( 0 ,0 ,255)', 'foo[bar]baz'],
        ['rgba(0, 0, 255, 0.0)', 'foo[bar]baz'],
        ['rgb(15, -10, 375)', 'foo[bar]baz'],
        ['rgba(0, 0, 0, 1)', 'foo[bar]baz'],
        ['rgba(255, 255, 255, 1)', 'foo[bar]baz'],
        ['rgba(0, 0, 255, 0.5)', 'foo[bar]baz'],
        ['hsl(240, 100%, 50%)', 'foo[bar]baz'],
        ['cornsilk', 'foo[bar]baz'],
        ['potato quiche', 'foo[bar]baz'],
        ['transparent', 'foo[bar]baz'],
        ['currentColor', 'foo[bar]baz'],

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<font color=blue>[bar]</font>baz',
        'foo{<font color=blue>bar</font>}baz',
        '<span style="color: blue">foo<span style="color: brown">[bar]</span>baz</span>',
        '<span style="color: #00f">foo<span style="color: brown">[bar]</span>baz</span>',
        '<span style="color: #0000ff">foo<span style="color: brown">[bar]</span>baz</span>',
        '<span style="color: rgb(0, 0, 255)">foo<span style="color: brown">[bar]</span>baz</span>',
        '<font color=blue>foo<font color=brown>[bar]</font>baz</font>',
        '<span style="color: rgb(0, 0, 255)">foo<span style="color: brown">b[ar]</span>baz</span>',
        'foo<span id=purple>ba[r</span>ba]z',
        '<span style="color: rgb(0, 0, 255)">foo<span id=purple>b[a]r</span>baz</span>',

        ['blue', '<a href=http://www.google.com>foo[bar]baz</a>'],
        ['#0000ff', '<a href=http://www.google.com>foo[bar]baz</a>'],
        ['rgb(0,0,255)', '<a href=http://www.google.com>foo[bar]baz</a>'],

        // Tests for queryCommandValue()
        '<font color="blue">[foo]</font>',
        '<font color="0000ff">[foo]</font>',
        '<font color="#0000ff">[foo]</font>',
        '<span style="color: blue">[foo]</span>',
        '<span style="color: #0000ff">[foo]</span>',
        '<span style="color: rgb(0, 0, 255)">[foo]</span>',
        '<span style="color: rgb(0%, 0%, 100%)">[foo]</span>',
        '<span style="color: rgb( 0 ,0 ,255)">[foo]</span>',
        '<span style="color: rgba(0, 0, 255, 0.0)">[foo]</span>',
        '<span style="color: rgb(15, -10, 375)">[foo]</span>',
        '<span style="color: rgba(0, 0, 0, 1)">[foo]</span>',
        '<span style="color: rgba(255, 255, 255, 1)">[foo]</span>',
        '<span style="color: rgba(0, 0, 255, 0.5)">[foo]</span>',
        '<span style="color: hsl(240, 100%, 50%)">[foo]</span>',
        '<span style="color: cornsilk">[foo]</span>',
        '<span style="color: transparent">[foo]</span>',
        '<span style="color: currentColor">[foo]</span>',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<font color=brown>b]ar</font>baz',
        'foo<font color=brown>ba[r</font>b]az',
        'fo[o<font color=brown>bar</font>b]az',
        'foo[<font color=brown>b]ar</font>baz',
        'foo<font color=brown>ba[r</font>]baz',
        'foo[<font color=brown>bar</font>]baz',
        'foo<font color=brown>[bar]</font>baz',
        'foo{<font color=brown>bar</font>}baz',
        '<font color=brown>fo[o</font><span style=color:brown>b]ar</span>',
        '<span style=color:brown>fo[o</span><span style=color:#0000ff>b]ar</span>',
    ],
    //@}
    formatblock: [
    //@{
        'foo[]bar<p>extra',
        '<span>foo</span>{}<span>bar</span><p>extra',
        '<span>foo[</span><span>]bar</span><p>extra',
        'foo[bar]baz<p>extra',
        'foo]bar[baz<p>extra',
        '{<p><p> <p>foo</p>}',
        'foo[bar<i>baz]qoz</i>quz<p>extra',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        '<div>[foobar]</div>',
        '<p>[foobar]</p>',
        '<blockquote>[foobar]</blockquote>',
        '<h1>[foobar]</h1>',
        '<h2>[foobar]</h2>',
        '<h3>[foobar]</h3>',
        '<h4>[foobar]</h4>',
        '<h5>[foobar]</h5>',
        '<h6>[foobar]</h6>',
        '<dl><dt>[foo]<dd>bar</dl>',
        '<dl><dt>foo<dd>[bar]</dl>',
        '<dl><dt>[foo<dd>bar]</dl>',
        '<ol><li>[foobar]</ol>',
        '<ul><li>[foobar]</ul>',
        '<address>[foobar]</address>',
        '<pre>[foobar]</pre>',
        '<article>[foobar]</article>',
        '<ins>[foobar]</ins>',
        '<del>[foobar]</del>',
        '<quasit>[foobar]</quasit>',
        '<quasit style="display: block">[foobar]</quasit>',

        ['<p>', 'foo[]bar<p>extra'],
        ['<p>', '<span>foo</span>{}<span>bar</span><p>extra'],
        ['<p>', '<span>foo[</span><span>]bar</span><p>extra'],
        ['<p>', 'foo[bar]baz<p>extra'],
        ['<p>', 'foo]bar[baz<p>extra'],
        ['<p>', '{<p><p> <p>foo</p>}'],
        ['<p>', 'foo[bar<i>baz]qoz</i>quz<p>extra'],

        ['<p>', '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>'],
        ['<p>', '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>'],
        ['<p>', '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>'],
        ['<p>', '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>'],
        ['<p>', '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>'],
        ['<p>', '{<table><tr><td>foo<td>bar<td>baz</table>}'],

        ['<p>', '<div>[foobar]</div>'],
        ['<p>', '<p>[foobar]</p>'],
        ['<p>', '<blockquote>[foobar]</blockquote>'],
        ['<p>', '<h1>[foobar]</h1>'],
        ['<p>', '<h2>[foobar]</h2>'],
        ['<p>', '<h3>[foobar]</h3>'],
        ['<p>', '<h4>[foobar]</h4>'],
        ['<p>', '<h5>[foobar]</h5>'],
        ['<p>', '<h6>[foobar]</h6>'],
        ['<p>', '<dl><dt>[foo]<dd>bar</dl>'],
        ['<p>', '<dl><dt>foo<dd>[bar]</dl>'],
        ['<p>', '<dl><dt>[foo<dd>bar]</dl>'],
        ['<p>', '<ol><li>[foobar]</ol>'],
        ['<p>', '<ul><li>[foobar]</ul>'],
        ['<p>', '<address>[foobar]</address>'],
        ['<p>', '<pre>[foobar]</pre>'],
        ['<p>', '<listing>[foobar]</listing>'],
        ['<p>', '<xmp>[foobar]</xmp>'],
        ['<p>', '<article>[foobar]</article>'],
        ['<p>', '<ins>[foobar]</ins>'],
        ['<p>', '<del>[foobar]</del>'],
        ['<p>', '<quasit>[foobar]</quasit>'],
        ['<p>', '<quasit style="display: block">[foobar]</quasit>'],

        ['<blockquote>', '<blockquote>[foo]</blockquote><p>extra'],
        ['<blockquote>', '<blockquote><p>[foo]<p>bar</blockquote><p>extra'],
        ['<blockquote>', '[foo]<blockquote>bar</blockquote><p>extra'],
        ['<blockquote>', '<p>[foo<p>bar]<p>baz'],
        ['<blockquote>', '<section>[foo]</section>'],
        ['<blockquote>', '<section><p>[foo]</section>'],
        ['<blockquote>', '<section><hgroup><h1>[foo]</h1><h2>bar</h2></hgroup><p>baz</section>'],
        ['<article>', '<section>[foo]</section>'],

        ['<address>', '<div>[foobar]</div>'],
        ['<article>', '<div>[foobar]</div>'],
        ['<blockquote>', '<div>[foobar]</div>'],
        ['<dd>', '<div>[foobar]</div>'],
        ['<del>', '<div>[foobar]</div>'],
        ['<dl>', '<div>[foobar]</div>'],
        ['<dt>', '<div>[foobar]</div>'],
        ['<h1>', '<div>[foobar]</div>'],
        ['<h2>', '<div>[foobar]</div>'],
        ['<h3>', '<div>[foobar]</div>'],
        ['<h4>', '<div>[foobar]</div>'],
        ['<h5>', '<div>[foobar]</div>'],
        ['<h6>', '<div>[foobar]</div>'],
        ['<ins>', '<div>[foobar]</div>'],
        ['<li>', '<div>[foobar]</div>'],
        ['<ol>', '<div>[foobar]</div>'],
        ['<pre>', '<div>[foobar]</div>'],
        ['<ul>', '<div>[foobar]</div>'],
        ['<quasit>', '<div>[foobar]</div>'],

        ['<address>', '<p>[foobar]</p>'],
        ['<article>', '<p>[foobar]</p>'],
        ['<aside>', '<p>[foobar]</p>'],
        ['<blockquote>', '<p>[foobar]</p>'],
        ['<body>', '<p>[foobar]</p>'],
        ['<dd>', '<p>[foobar]</p>'],
        ['<del>', '<p>[foobar]</p>'],
        ['<details>', '<p>[foobar]</p>'],
        ['<dir>', '<p>[foobar]</p>'],
        ['<dl>', '<p>[foobar]</p>'],
        ['<dt>', '<p>[foobar]</p>'],
        ['<fieldset>', '<p>[foobar]</p>'],
        ['<figcaption>', '<p>[foobar]</p>'],
        ['<figure>', '<p>[foobar]</p>'],
        ['<footer>', '<p>[foobar]</p>'],
        ['<form>', '<p>[foobar]</p>'],
        ['<h1>', '<p>[foobar]</p>'],
        ['<h2>', '<p>[foobar]</p>'],
        ['<h3>', '<p>[foobar]</p>'],
        ['<h4>', '<p>[foobar]</p>'],
        ['<h5>', '<p>[foobar]</p>'],
        ['<h6>', '<p>[foobar]</p>'],
        ['<header>', '<p>[foobar]</p>'],
        ['<head>', '<p>[foobar]</p>'],
        ['<hgroup>', '<p>[foobar]</p>'],
        ['<hr>', '<p>[foobar]</p>'],
        ['<html>', '<p>[foobar]</p>'],
        ['<ins>', '<p>[foobar]</p>'],
        ['<li>', '<p>[foobar]</p>'],
        ['<listing>', '<p>[foobar]</p>'],
        ['<menu>', '<p>[foobar]</p>'],
        ['<nav>', '<p>[foobar]</p>'],
        ['<ol>', '<p>[foobar]</p>'],
        ['<plaintext>', '<p>[foobar]</p>'],
        ['<pre>', '<p>[foobar]</p>'],
        ['<section>', '<p>[foobar]</p>'],
        ['<ul>', '<p>[foobar]</p>'],
        ['<xmp>', '<p>[foobar]</p>'],
        ['<quasit>', '<p>[foobar]</p>'],

        ['<address>', '<p>[foo<p>bar]'],
        ['<article>', '<p>[foo<p>bar]'],
        ['<aside>', '<p>[foo<p>bar]'],
        ['<blockquote>', '<p>[foo<p>bar]'],
        ['<body>', '<p>[foo<p>bar]'],
        ['<dd>', '<p>[foo<p>bar]'],
        ['<del>', '<p>[foo<p>bar]'],
        ['<details>', '<p>[foo<p>bar]'],
        ['<dir>', '<p>[foo<p>bar]'],
        ['<div>', '<p>[foo<p>bar]'],
        ['<dl>', '<p>[foo<p>bar]'],
        ['<dt>', '<p>[foo<p>bar]'],
        ['<fieldset>', '<p>[foo<p>bar]'],
        ['<figcaption>', '<p>[foo<p>bar]'],
        ['<figure>', '<p>[foo<p>bar]'],
        ['<footer>', '<p>[foo<p>bar]'],
        ['<form>', '<p>[foo<p>bar]'],
        ['<h1>', '<p>[foo<p>bar]'],
        ['<h2>', '<p>[foo<p>bar]'],
        ['<h3>', '<p>[foo<p>bar]'],
        ['<h4>', '<p>[foo<p>bar]'],
        ['<h5>', '<p>[foo<p>bar]'],
        ['<h6>', '<p>[foo<p>bar]'],
        ['<header>', '<p>[foo<p>bar]'],
        ['<head>', '<p>[foo<p>bar]'],
        ['<hgroup>', '<p>[foo<p>bar]'],
        ['<hr>', '<p>[foo<p>bar]'],
        ['<html>', '<p>[foo<p>bar]'],
        ['<ins>', '<p>[foo<p>bar]'],
        ['<li>', '<p>[foo<p>bar]'],
        ['<listing>', '<p>[foo<p>bar]'],
        ['<menu>', '<p>[foo<p>bar]'],
        ['<nav>', '<p>[foo<p>bar]'],
        ['<ol>', '<p>[foo<p>bar]'],
        ['<p>', '<p>[foo<p>bar]'],
        ['<plaintext>', '<p>[foo<p>bar]'],
        ['<pre>', '<p>[foo<p>bar]'],
        ['<section>', '<p>[foo<p>bar]'],
        ['<ul>', '<p>[foo<p>bar]'],
        ['<xmp>', '<p>[foo<p>bar]'],
        ['<quasit>', '<p>[foo<p>bar]'],

        ['p', '<div>[foobar]</div>'],

        '<ol><li>[foo]<li>bar</ol>',

        ['<p>', '<h1>[foo]<br>bar</h1>'],
        ['<p>', '<h1>foo<br>[bar]</h1>'],
        ['<p>', '<h1>[foo<br>bar]</h1>'],
        ['<address>', '<h1>[foo]<br>bar</h1>'],
        ['<address>', '<h1>foo<br>[bar]</h1>'],
        ['<address>', '<h1>[foo<br>bar]</h1>'],
        ['<pre>', '<h1>[foo]<br>bar</h1>'],
        ['<pre>', '<h1>foo<br>[bar]</h1>'],
        ['<pre>', '<h1>[foo<br>bar]</h1>'],
        ['<h2>', '<h1>[foo]<br>bar</h1>'],
        ['<h2>', '<h1>foo<br>[bar]</h1>'],
        ['<h2>', '<h1>[foo<br>bar]</h1>'],

        ['<h1>', '<p>[foo]<br>bar</p>'],
        ['<h1>', '<p>foo<br>[bar]</p>'],
        ['<h1>', '<p>[foo<br>bar]</p>'],
        ['<address>', '<p>[foo]<br>bar</p>'],
        ['<address>', '<p>foo<br>[bar]</p>'],
        ['<address>', '<p>[foo<br>bar]</p>'],
        ['<pre>', '<p>[foo]<br>bar</p>'],
        ['<pre>', '<p>foo<br>[bar]</p>'],
        ['<pre>', '<p>[foo<br>bar]</p>'],

        ['<p>', '<address>[foo]<br>bar</address>'],
        ['<p>', '<address>foo<br>[bar]</address>'],
        ['<p>', '<address>[foo<br>bar]</address>'],
        ['<pre>', '<address>[foo]<br>bar</address>'],
        ['<pre>', '<address>foo<br>[bar]</address>'],
        ['<pre>', '<address>[foo<br>bar]</address>'],
        ['<h1>', '<address>[foo]<br>bar</address>'],
        ['<h1>', '<address>foo<br>[bar]</address>'],
        ['<h1>', '<address>[foo<br>bar]</address>'],

        ['<p>', '<pre>[foo]<br>bar</pre>'],
        ['<p>', '<pre>foo<br>[bar]</pre>'],
        ['<p>', '<pre>[foo<br>bar]</pre>'],
        ['<address>', '<pre>[foo]<br>bar</pre>'],
        ['<address>', '<pre>foo<br>[bar]</pre>'],
        ['<address>', '<pre>[foo<br>bar]</pre>'],
        ['<h1>', '<pre>[foo]<br>bar</pre>'],
        ['<h1>', '<pre>foo<br>[bar]</pre>'],
        ['<h1>', '<pre>[foo<br>bar]</pre>'],

        ['<h1>', '<p>[foo</p>bar]'],
        ['<h1>', '[foo<p>bar]</p>'],
        ['<p>', '<div>[foo<p>bar]</p></div>'],
        ['<p>', '<xmp>[foo]</xmp>'],
        ['<div>', '<xmp>[foo]</xmp>'],

        '<div><ol><li>[foo]</ol></div>',
        '<div><table><tr><td>[foo]</table></div>',
        '<p>[foo<h1>bar]</h1>',
        '<h1>[foo</h1><h2>bar]</h2>',
        '<div>[foo</div>bar]',

        // https://bugs.webkit.org/show_bug.cgi?id=47054
        ['<p>', '<div style=color:blue>[foo]</div>'],
        // https://bugs.webkit.org/show_bug.cgi?id=47574
        ['<h1>', '{<p>foo</p>ba]r'],
        ['<pre>', '&#10;[foo<p>bar]</p>'],
        // From https://bugs.webkit.org/show_bug.cgi?id=47300
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=14009
        ['!<p>', '{<pre>&#10;foo&#10;&#10;bar&#10;</pre>}'],
    ],
    //@}
    forwarddelete: [
    //@{
        // Collapsed selection
        'foo[]',
        '<span>foo[]</span>',
        '<p>foo[]</p>',
        'foo[]bar',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[]<span style=display:none>bar</span>baz',
        'foo[]<script>bar</script>baz',
        'fo[]&ouml;bar',
        'fo[]o&#x308;bar',
        'fo[]o&#x308;&#x327;bar',
        '[]&ouml;bar',
        '[]o&#x308;bar',
        '[]o&#x308;&#x327;bar',

        '[]&#x5e9;&#x5c1;&#x5b8;&#x5dc;&#x5d5;&#x5b9;&#x5dd;',
        '&#x5e9;&#x5c1;&#x5b8;&#x5dc;[]&#x5d5;&#x5b9;&#x5dd;',

        '<p>foo[]</p><p>bar</p>',
        '<p>foo[]</p>bar',
        'foo[]<p>bar</p>',
        '<p>foo[]<br></p><p>bar</p>',
        '<p>foo[]<br></p>bar',
        'foo[]<br><p>bar</p>',

        '<p>{}<br></p>foo',
        '<p>{}<span><br></span></p>foo',
        'foo{}<p><br>',
        'foo{}<p><span><br></span>',
        'foo{}<br><p><br>',
        'foo{}<span><br></span><p><br>',
        'foo{}<br><p><span><br></span>',
        'foo{}<span><br></span><p><span><br></span>',
        'foo{}<p>',
        '<table><tr><td>{}</table>foo',
        '<table><tr><td>{}<br></table>foo',
        '<table><tr><td>{}<span><br></span></table>foo',

        '<div><p>foo[]</p></div><p>bar</p>',
        '<p>foo[]</p><div><p>bar</p></div>',
        '<div><p>foo[]</p></div><div><p>bar</p></div>',
        '<div><p>foo[]</p></div>bar',
        'foo[]<div><p>bar</p></div>',

        '<div>foo[]</div><div>bar</div>',
        '<pre>foo[]</pre>bar',

        'foo[]<br>bar',
        '<b>foo[]</b><br>bar',
        'foo[]<hr>bar',
        '<p>foo[]<hr><p>bar',
        '<p>foo[]</p><br><p>bar</p>',
        '<p>foo[]</p><br><br><p>bar</p>',
        '<p>foo[]</p><img src=/img/lion.svg><p>bar',
        'foo[]<img src=/img/lion.svg>bar',

        'foo[]<a>bar</a>',
        'foo[]<a href=/>bar</a>',
        'foo[]<a name=abc>bar</a>',
        'foo[]<a href=/ name=abc>bar</a>',
        'foo[]<span><a>bar</a></span>',
        'foo[]<span><a href=/>bar</a></span>',
        'foo[]<span><a name=abc>bar</a></span>',
        'foo[]<span><a href=/ name=abc>bar</a></span>',
        '<a>foo[]</a>bar',
        '<a href=/>foo[]</a>bar',
        '<a name=abc>foo[]</a>bar',
        '<a href=/ name=abc>foo[]</a>bar',

        'foo []&nbsp;',
        '[]&nbsp; foo',
        'foo[] &nbsp;bar',
        'foo[]&nbsp; bar',
        'foo[]&nbsp;&nbsp;bar',
        'foo[]  bar',
        'foo[] &nbsp; bar',
        'foo []&nbsp; bar',
        'foo &nbsp;[] bar',
        'foo[] <span>&nbsp;</span> bar',
        'foo []<span>&nbsp;</span> bar',
        'foo <span>&nbsp;</span>[] bar',
        '<b>foo[] </b>&nbsp;bar',
        '<b>foo[]&nbsp;</b> bar',
        '<b>foo[]&nbsp;</b>&nbsp;bar',
        '<b>foo[] </b> bar',

        '<pre>foo []&nbsp;</pre>',
        '<pre>[]&nbsp; foo</pre>',
        '<pre>foo[] &nbsp;bar</pre>',
        '<pre>foo[]&nbsp; bar</pre>',
        '<pre>foo[]  bar</pre>',

        '<div style=white-space:pre>foo []&nbsp;</div>',
        '<div style=white-space:pre>[]&nbsp; foo</div>',
        '<div style=white-space:pre>foo[] &nbsp;bar</div>',
        '<div style=white-space:pre>foo[]&nbsp; bar</div>',
        '<div style=white-space:pre>foo[]  bar</div>',

        '<div style=white-space:pre-wrap>foo []&nbsp;</div>',
        '<div style=white-space:pre-wrap>[]&nbsp; foo</div>',
        '<div style=white-space:pre-wrap>foo[] &nbsp;bar</div>',
        '<div style=white-space:pre-wrap>foo[]&nbsp; bar</div>',
        '<div style=white-space:pre-wrap>foo[]  bar</div>',

        '<div style=white-space:pre-line>foo []&nbsp;</div>',
        '<div style=white-space:pre-line>[]&nbsp; foo</div>',
        '<div style=white-space:pre-line>foo[] &nbsp;bar</div>',
        '<div style=white-space:pre-line>foo[]&nbsp; bar</div>',
        '<div style=white-space:pre-line>foo[]  bar</div>',

        '<div style=white-space:nowrap>foo []&nbsp;</div>',
        '<div style=white-space:nowrap>[]&nbsp; foo</div>',
        '<div style=white-space:nowrap>foo[] &nbsp;bar</div>',
        '<div style=white-space:nowrap>foo[]&nbsp; bar</div>',
        '<div style=white-space:nowrap>foo[]  bar</div>',

        // Tables with collapsed selection
        'foo[]<table><tr><td>bar</table>baz',
        'foo<table><tr><td>bar[]</table>baz',
        '<p>foo[]<table><tr><td>bar</table><p>baz',
        '<table><tr><td>foo[]<td>bar</table>',
        '<table><tr><td>foo[]<tr><td>bar</table>',

        'foo[]<br><table><tr><td>bar</table>baz',
        'foo<table><tr><td>bar[]<br></table>baz',
        '<p>foo[]<br><table><tr><td>bar</table><p>baz',
        '<p>foo<table><tr><td>bar[]<br></table><p>baz',
        '<table><tr><td>foo[]<br><td>bar</table>',
        '<table><tr><td>foo[]<br><tr><td>bar</table>',

        'foo<table><tr><td>bar[]</table><br>baz',
        'foo[]<table><tr><td><hr>bar</table>baz',
        '<table><tr><td>foo[]<td><hr>bar</table>',
        '<table><tr><td>foo[]<tr><td><hr>bar</table>',

        // Lists with collapsed selection
        'foo[]<ol><li>bar<li>baz</ol>',
        'foo[]<br><ol><li>bar<li>baz</ol>',
        '<ol><li>foo[]<li>bar</ol>',
        '<ol><li>foo[]<br><li>bar</ol>',
        '<ol><li>foo[]<li>bar<br>baz</ol>',

        '<ol><li><p>foo[]<li>bar</ol>',
        '<ol><li>foo[]<li><p>bar</ol>',
        '<ol><li><p>foo[]<li><p>bar</ol>',

        '<ol><li>foo[]<ul><li>bar</ul></ol>',
        'foo[]<ol><ol><li>bar</ol></ol>',
        'foo[]<div><ol><li>bar</ol></div>',

        'foo[]<dl><dt>bar<dd>baz</dl>',
        'foo[]<dl><dd>bar</dl>',
        '<dl><dt>foo[]<dd>bar</dl>',
        '<dl><dt>foo[]<dt>bar<dd>baz</dl>',
        '<dl><dt>foo<dd>bar[]<dd>baz</dl>',

        '<ol><li>foo[]</ol>bar',
        '<ol><li>foo[]<br></ol>bar',
        '<ol><li>{}<br></ol>bar',
        '<ol><li>foo<li>{}<br></ol>bar',

        '<ol><li>foo[]</ol><p>bar',
        '<ol><li>foo[]<br></ol><p>bar',
        '<ol><li>{}<br></ol><p>bar',
        '<ol><li>foo<li>{}<br></ol><p>bar',

        '<ol><li>foo[]</ol><br>',
        '<ol><li>foo[]<br></ol><br>',
        '<ol><li>{}<br></ol><br>',
        '<ol><li>foo<li>{}<br></ol><br>',

        '<ol><li>foo[]</ol><p><br>',
        '<ol><li>foo[]<br></ol><p><br>',
        '<ol><li>{}<br></ol><p><br>',
        '<ol><li>foo<li>{}<br></ol><p><br>',

        // Indented stuff with collapsed selection
        'foo[]<blockquote>bar</blockquote>',
        'foo[]<blockquote><blockquote>bar</blockquote></blockquote>',
        'foo[]<blockquote><div>bar</div></blockquote>',
        'foo[]<blockquote style="color: blue">bar</blockquote>',

        'foo[]<blockquote><blockquote><p>bar<p>baz</blockquote></blockquote>',
        'foo[]<blockquote><div><p>bar<p>baz</div></blockquote>',
        'foo[]<blockquote style="color: blue"><p>bar<p>baz</blockquote>',

        'foo[]<blockquote><p><b>bar</b><p>baz</blockquote>',
        'foo[]<blockquote><p><strong>bar</strong><p>baz</blockquote>',
        'foo[]<blockquote><p><span>bar</span><p>baz</blockquote>',

        'foo[]<blockquote><ol><li>bar</ol></blockquote><p>extra',
        'foo[]<blockquote>bar<ol><li>baz</ol>quz</blockquote><p>extra',
        'foo<blockquote><ol><li>bar[]</li><ol><li>baz</ol><li>quz</ol></blockquote><p>extra',

        // Invisible stuff with collapsed selection
        'foo[]<span></span>bar',
        'foo[]<span><span></span></span>bar',
        'foo[]<quasit></quasit>bar',
        'foo[]<span></span><br>bar',
        '<span>foo[]<span></span></span>bar',
        'foo[]<span></span><span>bar</span>',
        'foo[]<div><div><p>bar</div></div>',
        'foo[]<div><div><p><!--abc-->bar</div></div>',
        'foo[]<div><div><!--abc--><p>bar</div></div>',
        'foo[]<div><!--abc--><div><p>bar</div></div>',
        'foo[]<!--abc--><div><div><p>bar</div></div>',
        '<div><div><p>foo[]</div></div>bar',
        '<div><div><p>foo[]</div></div><!--abc-->bar',
        '<div><div><p>foo[]</div><!--abc--></div>bar',
        '<div><div><p>foo[]</p><!--abc--></div></div>bar',
        '<div><div><p>foo[]<!--abc--></div></div>bar',
        '<div><div><p>foo[]</p></div></div><div><div><div>bar</div></div></div>',
        '<div><div><p>foo[]<!--abc--></p></div></div><div><div><div>bar</div></div></div>',
        '<div><div><p>foo[]</p><!--abc--></div></div><div><div><div>bar</div></div></div>',
        '<div><div><p>foo[]</p></div><!--abc--></div><div><div><div>bar</div></div></div>',
        '<div><div><p>foo[]</p></div></div><!--abc--><div><div><div>bar</div></div></div>',
        '<div><div><p>foo[]</p></div></div><div><!--abc--><div><div>bar</div></div></div>',
        '<div><div><p>foo[]</p></div></div><div><div><!--abc--><div>bar</div></div></div>',
        '<div><div><p>foo[]</p></div></div><div><div><div><!--abc-->bar</div></div></div>',

        // Styled stuff with collapsed selection
        '<p style=color:blue>foo[]<p>bar',
        '<p style=color:blue>foo[]<p style=color:brown>bar',
        '<p>foo[]<p style=color:brown>bar',
        '<p><font color=blue>foo[]</font><p>bar',
        '<p><font color=blue>foo[]</font><p><font color=brown>bar</font>',
        '<p>foo[]<p><font color=brown>bar</font>',
        '<p><span style=color:blue>foo[]</font><p>bar',
        '<p><span style=color:blue>foo[]</font><p><span style=color:brown>bar</font>',
        '<p>foo[]<p><span style=color:brown>bar</font>',

        '<p style=background-color:aqua>foo[]<p>bar',
        '<p style=background-color:aqua>foo[]<p style=background-color:tan>bar',
        '<p>foo[]<p style=background-color:tan>bar',
        '<p><span style=background-color:aqua>foo[]</font><p>bar',
        '<p><span style=background-color:aqua>foo[]</font><p><span style=background-color:tan>bar</font>',
        '<p>foo[]<p><span style=background-color:tan>bar</font>',

        '<p style=text-decoration:underline>foo[]<p>bar',
        '<p style=text-decoration:underline>foo[]<p style=text-decoration:line-through>bar',
        '<p>foo[]<p style=text-decoration:line-through>bar',
        '<p><u>foo[]</u><p>bar',
        '<p><u>foo[]</u><p><s>bar</s>',
        '<p>foo[]<p><s>bar</s>',

        '<p style=color:blue>foo[]</p>bar',
        'foo[]<p style=color:brown>bar',
        '<div style=color:blue><p style=color:green>foo[]</div>bar',
        '<div style=color:blue><p style=color:green>foo[]</div><p style=color:brown>bar',
        '<p style=color:blue>foo[]<div style=color:brown><p style=color:green>bar',

        // Uncollapsed selection (should be same as delete command)
        'foo[bar]baz',
        '<p>foo<span style=color:#aBcDeF>[bar]</span>baz',
        '<p>foo<span style=color:#aBcDeF>{bar}</span>baz',
        '<p>foo{<span style=color:#aBcDeF>bar</span>}baz',
        '<p>[foo<span style=color:#aBcDeF>bar]</span>baz',
        '<p>{foo<span style=color:#aBcDeF>bar}</span>baz',
        '<p>foo<span style=color:#aBcDeF>[bar</span>baz]',
        '<p>foo<span style=color:#aBcDeF>{bar</span>baz}',
        '<p>foo<span style=color:#aBcDeF>[bar</span><span style=color:#fEdCbA>baz]</span>quz',

        'foo<b>[bar]</b>baz',
        'foo<b>{bar}</b>baz',
        'foo{<b>bar</b>}baz',
        'foo<span>[bar]</span>baz',
        'foo<span>{bar}</span>baz',
        'foo{<span>bar</span>}baz',
        '<b>foo[bar</b><i>baz]quz</i>',
        '<p>foo</p><p>[bar]</p><p>baz</p>',
        '<p>foo</p><p>{bar}</p><p>baz</p>',
        '<p>foo</p><p>{bar</p>}<p>baz</p>',
        '<p>foo</p>{<p>bar}</p><p>baz</p>',
        '<p>foo</p>{<p>bar</p>}<p>baz</p>',

        '<p>foo[bar<p>baz]quz',
        '<p>foo[bar<div>baz]quz</div>',
        '<p>foo[bar<h1>baz]quz</h1>',
        '<div>foo[bar</div><p>baz]quz',
        '<blockquote>foo[bar</blockquote><pre>baz]quz</pre>',

        '<p><b>foo[bar</b><p>baz]quz',
        '<div><p>foo[bar</div><p>baz]quz',
        '<p>foo[bar<blockquote><p>baz]quz<p>qoz</blockquote',
        '<p>foo[bar<p style=color:blue>baz]quz',
        '<p>foo[bar<p><b>baz]quz</b>',

        '<div><p>foo<p>[bar<p>baz]</div>',

        'foo[<br>]bar',
        '<p>foo[</p><p>]bar</p>',
        '<p>foo[</p><p>]bar<br>baz</p>',
        'foo[<p>]bar</p>',
        'foo{<p>}bar</p>',
        'foo[<p>]bar<br>baz</p>',
        'foo[<p>]bar</p>baz',
        'foo{<p>bar</p>}baz',
        'foo<p>{bar</p>}baz',
        'foo{<p>bar}</p>baz',
        '<p>foo[</p>]bar',
        '<p>foo{</p>}bar',
        '<p>foo[</p>]bar<br>baz',
        '<p>foo[</p>]bar<p>baz</p>',
        'foo[<div><p>]bar</div>',
        '<div><p>foo[</p></div>]bar',
        'foo[<div><p>]bar</p>baz</div>',
        'foo[<div>]bar<p>baz</p></div>',
        '<div><p>foo</p>bar[</div>]baz',
        '<div>foo<p>bar[</p></div>]baz',

        '<p>foo<br>{</p>]bar',
        '<p>foo<br><br>{</p>]bar',
        'foo<br>{<p>]bar</p>',
        'foo<br><br>{<p>]bar</p>',
        '<p>foo<br>{</p><p>}bar</p>',
        '<p>foo<br><br>{</p><p>}bar</p>',

        '<table><tbody><tr><th>foo<th>[bar]<th>baz<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>foo<th>ba[r<th>b]az<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>fo[o<th>bar<th>b]az<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>foo<th>bar<th>ba[z<tr><td>q]uz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>[foo<th>bar<th>baz]<tr><td>quz<td>qoz<td>qiz</table>',
        '<table><tbody><tr><th>[foo<th>bar<th>baz<tr><td>quz<td>qoz<td>qiz]</table>',
        '{<table><tbody><tr><th>foo<th>bar<th>baz<tr><td>quz<td>qoz<td>qiz</table>}',
        '<table><tbody><tr><td>foo<td>ba[r<tr><td>baz<td>quz<tr><td>q]oz<td>qiz</table>',
        '<p>fo[o<table><tr><td>b]ar</table><p>baz',
        '<p>foo<table><tr><td>ba[r</table><p>b]az',
        '<p>fo[o<table><tr><td>bar</table><p>b]az',

        '<p>foo<ol><li>ba[r<li>b]az</ol><p>quz',
        '<p>foo<ol><li>bar<li>[baz]</ol><p>quz',
        '<p>fo[o<ol><li>b]ar<li>baz</ol><p>quz',
        '<p>foo<ol><li>bar<li>ba[z</ol><p>q]uz',
        '<p>fo[o<ol><li>bar<li>b]az</ol><p>quz',
        '<p>fo[o<ol><li>bar<li>baz</ol><p>q]uz',

        '<ol><li>fo[o</ol><ol><li>b]ar</ol>',
        '<ol><li>fo[o</ol><ul><li>b]ar</ul>',

        'foo[<ol><li>]bar</ol>',
        '<ol><li>foo[<li>]bar</ol>',
        'foo[<dl><dt>]bar<dd>baz</dl>',
        'foo[<dl><dd>]bar</dl>',
        '<dl><dt>foo[<dd>]bar</dl>',
        '<dl><dt>foo[<dt>]bar<dd>baz</dl>',
        '<dl><dt>foo<dd>bar[<dd>]baz</dl>',

        // https://bugs.webkit.org/show_bug.cgi?id=35281
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=13976
        '<ol><li>foo</ol>{}<br><ol><li>bar</ol>',
        '<ol><li>foo</ol><p>{}<br></p><ol><li>bar</ol>',
        '<ol><li><p>foo</ol><p>{}<br></p><ol><li>bar</ol>',
        '<ol id=a><li>foo</ol>{}<br><ol><li>bar</ol>',
        '<ol><li>foo</ol>{}<br><ol id=b><li>bar</ol>',
        '<ol id=a><li>foo</ol>{}<br><ol id=b><li>bar</ol>',
        '<ol class=a><li>foo</ol>{}<br><ol class=b><li>bar</ol>',
        '<ol><ol><li>foo</ol><li>{}<br><ol><li>bar</ol></ol>',
        '<ol><ol><li>foo</ol><li>{}<br></li><ol><li>bar</ol></ol>',
        '<ol><li>foo[</ol>bar]<ol><li>baz</ol>',
        '<ol><li>foo[</ol><p>bar]<ol><li>baz</ol>',
        '<ol><li><p>foo[</ol><p>bar]<ol><li>baz</ol>',
        '<ol><li>fo[]o</ol><ol><li>bar</ol>',
        '<ol><li>foo</ol>[bar<ol><li>]baz</ol>',
        '<ol><li>foo</ol><p>[bar<ol><li>]baz</ol>',
        '<ol><li>foo</ol><p>[bar<ol><li><p>]baz</ol>',
        '<ol><li>foo</ol><ol><li>[]bar</ol>',
        '<ol><ol><li>foo[</ol><li>bar</ol>baz]<ol><li>quz</ol>',
        '<ul><li>foo</ul>{}<br><ul><li>bar</ul>',
        '<ul><li>foo</ul><p>{}<br></p><ul><li>bar</ul>',
        '<ol><li>foo[<li>bar]</ol><ol><li>baz</ol><ol><li>quz</ol>',
        '<ol><li>foo</ol>{}<br><ul><li>bar</ul>',
        '<ol><li>foo</ol><p>{}<br></p><ul><li>bar</ul>',
        '<ul><li>foo</ul>{}<br><ol><li>bar</ol>',
        '<ul><li>foo</ul><p>{}<br></p><ol><li>bar</ol>',

        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=13831
        '<p><b>[foo]</b>',
        '<p><quasit>[foo]</quasit>',
        '<p><b><i>[foo]</i></b>',
        '<p><b>{foo}</b>',
        '<p>{<b>foo</b>}',
        '<p><b>[]f</b>',
        '<b>[foo]</b>',
        '<div><b>[foo]</b></div>',
    ],
    //@}
    hilitecolor: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        '<p style="background-color: rgb(0, 255, 255)">foo[bar]baz</p>',
        '<p style="background-color: #00ffff">foo[bar]baz</p>',
        '<p style="background-color: aqua">foo[bar]baz</p>',
        '{<p style="background-color: aqua">foo</p><p>bar</p>}',
        '<span style="background-color: aqua">foo<span style="background-color: tan">[bar]</span>baz</span>',
        '<span style="background-color: #00ffff">foo<span style="background-color: tan">[bar]</span>baz</span>',
        '<span style="background-color: #0ff">foo<span style="background-color: tan">[bar]</span>baz</span>',
        '<span style="background-color: rgb(0, 255, 255)">foo<span style="background-color: tan">[bar]</span>baz</span>',
        '<span style="background-color: aqua">foo<span style="background-color: tan">b[ar]</span>baz</span>',
        '<p style="background-color: aqua">foo<span style="background-color: tan">b[ar]</span>baz</p>',
        '<div style="background-color: aqua"><p style="background-color: tan">b[ar]</p></div>',
        '<span style="display: block; background-color: aqua"><span style="display: block; background-color: tan">b[ar]</span></span>',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<span style=background-color:tan>b]ar</span>baz',
        'foo<span style=background-color:tan>ba[r</span>b]az',
        'fo[o<span style=background-color:tan>bar</span>b]az',
        'foo[<span style=background-color:tan>b]ar</span>baz',
        'foo<span style=background-color:tan>ba[r</span>]baz',
        'foo[<span style=background-color:tan>bar</span>]baz',
        'foo<span style=background-color:tan>[bar]</span>baz',
        'foo{<span style=background-color:tan>bar</span>}baz',
        '<span style=background-color:tan>fo[o</span><span style=background-color:yellow>b]ar</span>',
        '<span style=background-color:tan>fo[o</span><span style=background-color:tan>b]ar</span>',
        '<span style=background-color:tan>fo[o<span style=background-color:transparent>b]ar</span></span>',

        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=13829
        '!<font size=6>[foo]</font>',
        '!<span style=font-size:xx-large>[foo]</span>',
        '!<font size=6>foo[bar]baz</font>',
        '!<span style=font-size:xx-large>foo[bar]baz</span>',
        '![foo<font size=6>bar</font>baz]',
        '![foo<span style=font-size:xx-large>bar</span>baz]',
    ],
    //@}
    indent: [
    //@{
        // All these have a trailing unselected paragraph, because otherwise
        // Gecko is unhappy: it throws exceptions in non-CSS mode, and in CSS
        // mode it adds the indentation invisibly to the wrapper div in many
        // cases.
        'foo[]bar<p>extra',
        '<span>foo</span>{}<span>bar</span><p>extra',
        '<span>foo[</span><span>]bar</span><p>extra',
        'foo[bar]baz<p>extra',
        '<p dir=rtl>פו[בר]בז<p dir=rtl>נוםף',
        '<p dir=rtl>פו[ברבז<p>Foobar]baz<p>Extra',
        '<p>Foo[barbaz<p dir=rtl>פובר]בז<p>Extra',
        '<div><p>Foo[barbaz<p dir=rtl>פובר]בז</div><p>Extra',
        'foo]bar[baz<p>extra',
        '{<p><p> <p>foo</p>}<p>extra',
        'foo[bar<i>baz]qoz</i>quz<p>extra',
        '[]foo<p>extra',
        'foo[]<p>extra',
        '<p>[]foo<p>extra',
        '<p>foo[]<p>extra',
        '<p>{}<br>foo</p><p>extra',
        '<p>foo<br>{}</p><p>extra',
        '<span>{}<br>foo</span>bar<p>extra',
        '<span>foo<br>{}</span>bar<p>extra',
        '<p>foo</p>{}<p>bar</p>',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<p>foo[bar]</p><p>baz</p><p>extra',
        '<p>[foobar</p><p>ba]z</p><p>extra',
        'foo[bar]<br>baz<p>extra',
        'foo[bar]<br><br><br><br>baz<p>extra',
        'foobar<br>[ba]z<p>extra',
        'foobar<br><br><br><br>[ba]z<p>extra',
        'foo[bar<br>ba]z<p>extra',
        '<div>foo<p>[bar]</p>baz</div><p>extra',

        // These mimic existing indentation in various browsers, to see how
        // they cope with indenting twice.  This is spec, Gecko non-CSS, and
        // Opera:
        '<blockquote><p>foo[bar]</p><p>baz</p></blockquote><p>extra',
        '<blockquote><p>foo[bar</p><p>b]az</p></blockquote><p>extra',
        '<blockquote><p>foo[bar]</p></blockquote><p>baz</p><p>extra',
        '<blockquote><p>foo[bar</p></blockquote><p>b]az</p><p>extra',
        '<p>[foo]<blockquote><p>bar</blockquote><p>extra',
        '<p>[foo<blockquote><p>b]ar</blockquote><p>extra',
        '<p>foo<blockquote><p>bar</blockquote><p>[baz]<p>extra',
        '<p>foo<blockquote><p>[bar</blockquote><p>baz]<p>extra',
        '<p>[foo<blockquote><p>bar</blockquote><p>baz]<p>extra',
        '<blockquote><p>foo</blockquote><p>[bar]<blockquote><p>baz</blockquote><p>extra',

        '<blockquote>foo[bar]<br>baz</blockquote><p>extra',
        '<blockquote>foo[bar<br>b]az</blockquote><p>extra',
        '<blockquote>foo[bar]</blockquote>baz<p>extra',
        '<blockquote>foo[bar</blockquote>b]az<p>extra',
        '[foo]<blockquote>bar</blockquote><p>extra',
        '[foo<blockquote>b]ar</blockquote><p>extra',
        'foo<blockquote>bar</blockquote>[baz]<p>extra',
        '[foo<blockquote>bar</blockquote>baz]<p>extra',
        '<blockquote>foo</blockquote>[bar]<blockquote>baz</blockquote><p>extra',

        // IE:
        '<blockquote style="margin-right: 0" dir="ltr"><p>foo[bar]</p><p>baz</p></blockquote><p>extra',
        '<blockquote style="margin-right: 0" dir="ltr"><p>foo[bar</p><p>b]az</p></blockquote><p>extra',
        '<blockquote style="margin-right: 0" dir="ltr"><p>foo[bar]</p></blockquote><p>baz</p><p>extra',
        '<blockquote style="margin-right: 0" dir="ltr"><p>foo[bar</p></blockquote><p>b]az</p><p>extra',
        '<p>[foo]<blockquote style="margin-right: 0" dir="ltr"><p>bar</blockquote><p>extra',
        '<p>[foo<blockquote style="margin-right: 0" dir="ltr"><p>b]ar</blockquote><p>extra',
        '<p>foo<blockquote style="margin-right: 0" dir="ltr"><p>bar</blockquote><p>[baz]<p>extra',
        '<p>foo<blockquote style="margin-right: 0" dir="ltr"><p>[bar</blockquote><p>baz]<p>extra',
        '<p>[foo<blockquote style="margin-right: 0" dir="ltr"><p>bar</blockquote><p>baz]<p>extra',
        '<blockquote style="margin-right: 0" dir="ltr"><p>foo</blockquote><p>[bar]<blockquote style="margin-right: 0" dir="ltr"><p>baz</blockquote><p>extra',

        // Firefox CSS mode:
        '<p style="margin-left: 40px">foo[bar]</p><p style="margin-left: 40px">baz</p><p>extra',
        '<p style="margin-left: 40px">foo[bar</p><p style="margin-left: 40px">b]az</p><p>extra',
        '<p style="margin-left: 40px">foo[bar]</p><p>baz</p><p>extra',
        '<p style="margin-left: 40px">foo[bar</p><p>b]az</p><p>extra',
        '<p>[foo]<p style="margin-left: 40px">bar<p>extra',
        '<p>[foo<p style="margin-left: 40px">b]ar<p>extra',
        '<p>foo<p style="margin-left: 40px">bar<p>[baz]<p>extra',
        '<p>foo<p style="margin-left: 40px">[bar<p>baz]<p>extra',
        '<p>[foo<p style="margin-left: 40px">bar<p>baz]<p>extra',
        '<p style="margin-left: 40px">foo<p>[bar]<p style="margin-left: 40px">baz<p>extra',

        // WebKit:
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>foo[bar]</p><p>baz</p></blockquote><p>extra',
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>foo[bar</p><p>b]az</p></blockquote><p>extra',
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>foo[bar]</p></blockquote><p>baz</p><p>extra',
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>foo[bar</p></blockquote><p>b]az</p><p>extra',
        '<p>[foo]<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>bar</blockquote><p>extra',
        '<p>[foo<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>b]ar</blockquote><p>extra',
        '<p>foo<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>bar</blockquote><p>[baz]<p>extra',
        '<p>foo<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>[bar</blockquote><p>baz]<p>extra',
        '<p>[foo<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>bar</blockquote><p>baz]<p>extra',
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>foo</blockquote><p>[bar]<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px"><p>baz</blockquote><p>extra',

        // MDC says "In Firefox, if the selection spans multiple lines at
        // different levels of indentation, only the least indented lines in
        // the selection will be indented."  Let's test that.
        '<blockquote>f[oo<blockquote>b]ar</blockquote></blockquote><p>extra',

        // Lists!
        '<ol><li>foo<li>[bar]<li>baz</ol>',
        '<ol data-start=1 data-end=2><li>foo<li>bar<li>baz</ol>',
        '<ol><li>foo</ol>[bar]',
        '<ol><li>[foo]<br>bar<li>baz</ol>',
        '<ol><li>foo<br>[bar]<li>baz</ol>',
        '<ol><li><div>[foo]</div>bar<li>baz</ol>',
        '<ol><li>foo<ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo</li><ol data-start=0 data-end=1><li>bar<li>baz</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol data-start=1 data-end=2><li>bar<li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>b[a]r</ol><li>baz</ol>',
        '<ol><li>foo</li><ol><li>b[a]r</ol><li>baz</ol>',
        '<ol><li>foo{<ol><li>bar</ol>}<li>baz</ol>',
        '<ol><li>foo</li>{<ol><li>bar</ol>}<li>baz</ol>',
        '<ol><li>[foo]<ol><li>bar</ol><li>baz</ol>',
        '<ol><li>[foo]</li><ol><li>bar</ol><li>baz</ol>',
        '<ol><li>foo<li>[bar]<ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<li>[bar]</li><ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>baz</ol><li>[quz]</ol>',
        '<ol><li>foo</li><ol><li>bar<li>baz</ol><li>[quz]</ol>',

        // Lists with id's:
        // http://lists.whatwg.org/pipermail/whatwg-whatwg.org/2009-July/020721.html
        '<ol><ol id=u1><li id=i1>foo</ol><li id=i2>[bar]</li><ol id=u3><li id=i3>baz</ol></ol>',
        '<ol><ol><li id=i1>foo</ol><li id=i2>[bar]</li><ol id=u3><li id=i3>baz</ol></ol>',
        '<ol><ol id=u1><li id=i1>foo</ol><li id=i2>[bar]</li><ol><li id=i3>baz</ol></ol>',
        '<ol><li id=i2>[bar]</li><ol id=u3><li id=i3>baz</ol></ol>',
        '<ol><ol id=u1><li id=i1>foo</ol><li id=i2>[bar]</ol>',

        // Try indenting multiple items at once.
        '<ol><li>foo<li>b[ar<li>baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol><li>baz</ol>',
        '<ol><li>[foo</li><ol><li>bar]</ol><li>baz</ol>',
        '<ol><li>foo<ol><li>b[ar</ol><li>b]az</ol>',
        '<ol><li>foo</li><ol><li>b[ar</ol><li>b]az</ol>',
        '<ol><li>[foo<ol><li>bar</ol><li>baz]</ol><p>extra',
        '<ol><li>[foo</li><ol><li>bar</ol><li>baz]</ol><p>extra',

        // We probably can't actually get this DOM . . .
        '<ol><li>[foo]<ol><li>bar</ol>baz</ol>',
        '<ol><li>foo<ol><li>[bar]</ol>baz</ol>',
        '<ol><li>foo<ol><li>bar</ol>[baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol>baz</ol>',

        'foo<!--bar-->[baz]<p>extra',
        '[foo]<!--bar-->baz<p>extra',
        '<p>foo<!--bar-->{}<p>extra',
        '<p>{}<!--foo-->bar<p>extra',

        // Whitespace nodes
        '<blockquote><p>foo</blockquote> <p>[bar]',
        '<p>[foo]</p> <blockquote><p>bar</blockquote>',
        '<blockquote><p>foo</blockquote> <p>[bar]</p> <blockquote><p>baz</blockquote>',
        '<ol><li>foo</li><ol><li>bar</li> </ol><li>[baz]</ol>',
        '<ol><li>foo</li><ol><li>bar</li></ol> <li>[baz]</ol>',
        '<ol><li>foo</li><ol><li>bar</li> </ol> <li>[baz]</ol>',
        '<ol><li>foo<ol><li>bar</li> </ol></li><li>[baz]</ol>',
        '<ol><li>foo<ol><li>bar</li></ol></li> <li>[baz]</ol>',
        '<ol><li>foo<ol><li>bar</li> </ol></li> <li>[baz]</ol>',
        '<ol><li>foo<li>[bar]</li> <ol><li>baz</ol></ol>',
        '<ol><li>foo<li>[bar]</li><ol> <li>baz</ol></ol>',
        '<ol><li>foo<li>[bar]</li> <ol> <li>baz</ol></ol>',
        '<ol><li>foo<li>[bar] <ol><li>baz</ol></ol>',
        '<ol><li>foo<li>[bar]<ol> <li>baz</ol></ol>',
        '<ol><li>foo<li>[bar] <ol> <li>baz</ol></ol>',

        // https://bugs.webkit.org/show_bug.cgi?id=32003
        '<ul><li>a<br>{<br>}</li><li>b</li></ul>',
    ],
    //@}
    inserthorizontalrule: [
    //@{
        'foo[]bar',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        '<p>foo[bar<p>baz]quz',
        '<div><b>foo</b>{}<b>bar</b></div>',
        '<div><b>foo[</b><b>]bar</b></div>',
        '<div><b>foo</b>{<b>bar</b>}<b>baz</b></div>',
        '<b>foo[]bar</b>',
        '<b id=abc>foo[]bar</b>',
        ["abc", 'foo[bar]baz'],
        'foo[bar]baz',

        'foo<b>[bar]</b>baz',
        'foo<b>{bar}</b>baz',
        'foo{<b>bar</b>}baz',
        '<p>foo<p>[bar]<p>baz',
        '<p>foo<p>{bar}<p>baz',
        '<p>foo{<p>bar</p>}<p>baz',

        '<p>foo[bar]baz</p>',
        '<p id=abc>foo[bar]baz</p>',
        '<h1>foo[bar]baz</h1>',
        '<p>foo<b>b[a]r</b>baz</p>',

        '<a>foo[bar]baz</a>',
        '<a href=/>foo[bar]baz</a>',
        '<abbr>foo[bar]baz</abbr>',
        '<address>foo[bar]baz</address>',
        '<article>foo[bar]baz</article>',
        '<aside>foo[bar]baz</aside>',
        '<b>foo[bar]baz</b>',
        '<bdi>foo[bar]baz</bdi>',
        '<bdo dir=rtl>foo[bar]baz</bdo>',
        '<blockquote>foo[bar]baz</blockquote>',
        '<table><caption>foo[bar]baz</caption><tr><td>quz</table>',
        '<cite>foo[bar]baz</cite>',
        '<code>foo[bar]baz</code>',
        '<dl><dd>foo[bar]baz</dd></dl>',
        '<del>foo[bar]baz</del>',
        '<details>foo[bar]baz</details>',
        '<dfn>foo[bar]baz</dfn>',
        '<div>foo[bar]baz</div>',
        '<dl><dt>foo[bar]baz</dt></dl>',
        '<em>foo[bar]baz</em>',
        '<figure><figcaption>foo[bar]baz</figcaption>quz</figure>',
        '<figure>foo[bar]baz</figure>',
        '<footer>foo[bar]baz</footer>',
        '<h1>foo[bar]baz</h1>',
        '<h2>foo[bar]baz</h2>',
        '<h3>foo[bar]baz</h3>',
        '<h4>foo[bar]baz</h4>',
        '<h5>foo[bar]baz</h5>',
        '<h6>foo[bar]baz</h6>',
        '<header>foo[bar]baz</header>',
        '<hgroup>foo[bar]baz</hgroup>',
        '<hgroup><h1>foo[bar]baz</h1></hgroup>',
        '<i>foo[bar]baz</i>',
        '<ins>foo[bar]baz</ins>',
        '<kbd>foo[bar]baz</kbd>',
        '<mark>foo[bar]baz</mark>',
        '<nav>foo[bar]baz</nav>',
        '<ol><li>foo[bar]baz</li></ol>',
        '<p>foo[bar]baz</p>',
        '<pre>foo[bar]baz</pre>',
        '<q>foo[bar]baz</q>',
        '<ruby>foo[bar]baz<rt>quz</rt></ruby>',
        '<ruby>foo<rt>bar[baz]quz</rt></ruby>',
        '<ruby>foo<rp>bar[baz]quz</rp><rt>qoz</rt><rp>qiz</rp></ruby>',
        '<s>foo[bar]baz</s>',
        '<samp>foo[bar]baz</samp>',
        '<section>foo[bar]baz</section>',
        '<small>foo[bar]baz</small>',
        '<span>foo[bar]baz</span>',
        '<strong>foo[bar]baz</strong>',
        '<sub>foo[bar]baz</sub>',
        '<sup>foo[bar]baz</sup>',
        '<table><tr><td>foo[bar]baz</td></table>',
        '<table><tr><th>foo[bar]baz</th></table>',
        '<u>foo[bar]baz</u>',
        '<ul><li>foo[bar]baz</li></ul>',
        '<var>foo[bar]baz</var>',

        '<acronym>foo[bar]baz</acronym>',
        '<big>foo[bar]baz</big>',
        '<blink>foo[bar]baz</blink>',
        '<center>foo[bar]baz</center>',
        '<dir>foo[bar]baz</dir>',
        '<dir><li>foo[bar]baz</li></dir>',
        '<font>foo[bar]baz</font>',
        '<listing>foo[bar]baz</listing>',
        '<marquee>foo[bar]baz</marquee>',
        '<nobr>foo[bar]baz</nobr>',
        '<strike>foo[bar]baz</strike>',
        '<tt>foo[bar]baz</tt>',
        '<xmp>foo[bar]baz</xmp>',

        '<quasit>foo[bar]baz</quasit>',

        '<table><tr><td>fo[o<td>b]ar</table>',
        'fo[o<span contenteditable=false>bar</span>b]az',
    ],
    //@}
    inserthtml: [
    //@{
        'foo[]bar',
        'foo[bar]baz',
        'foo<span style=color:#aBcDeF>[bar]</span>baz',
        'foo<span style=color:#aBcDeF>{bar}</span>baz',
        'foo{<span style=color:#aBcDeF>bar</span>}baz',
        '[foo<span style=color:#aBcDeF>bar]</span>baz',
        '{foo<span style=color:#aBcDeF>bar}</span>baz',
        'foo<span style=color:#aBcDeF>[bar</span>baz]',
        'foo<span style=color:#aBcDeF>{bar</span>baz}',
        'foo<span style=color:#aBcDeF>[bar</span><span style=color:#fEdCbA>baz]</span>quz',

        ['', 'foo[bar]baz'],
        ['\0', 'foo[bar]baz'],
        ['\x07', 'foo[bar]baz'],
        // The following line makes Firefox 7.0a2 go into an infinite loop on
        // my machine.
        //['\ud800', 'foo[bar]baz'],

        ['<b>', 'foo[bar]baz'],
        ['<b>abc', 'foo[bar]baz'],
        ['<p>abc', '<p>foo[bar]baz'],
        ['<li>abc', '<p>foo[bar]baz'],
        ['<p>abc', '<ol>{<li>foo</li>}<li>bar</ol>'],
        ['<p>abc', '<ol><li>foo</li>{<li>bar</li>}<li>baz</ol>'],
        ['<p>abc', '<ol><li>[foo]</li><li>bar</ol>'],

        ['abc', '<xmp>f[o]o</xmp>'],
        ['<b>abc</b>', '<xmp>f[o]o</xmp>'],
        ['abc', '<script>f[o]o</script>bar'],
        ['<b>abc</b>', '<script>f[o]o</script>bar'],

        ['<a>abc</a>', '<a>f[o]o</a>'],
        ['<a href=/>abc</a>', '<a href=.>f[o]o</a>'],
        ['<hr>', '<p>f[o]o'],
        ['<hr>', '<b>f[o]o</b>'],
        ['<h2>abc</h2>', '<h1>f[o]o</h1>'],
        ['<td>abc</td>', '<table><tr><td>f[o]o</table>'],
        ['<td>abc</td>', 'f[o]o'],

        ['<dt>abc</dt>', '<dl><dt>f[o]o<dd>bar</dl>'],
        ['<dt>abc</dt>', '<dl><dt>foo<dd>b[a]r</dl>'],
        ['<dd>abc</dd>', '<dl><dt>f[o]o<dd>bar</dl>'],
        ['<dd>abc</dd>', '<dl><dt>foo<dd>b[a]r</dl>'],
        ['<dt>abc</dt>', 'f[o]o'],
        ['<dt>abc</dt>', '<ol><li>f[o]o</ol>'],
        ['<dd>abc</dd>', 'f[o]o'],
        ['<dd>abc</dd>', '<ol><li>f[o]o</ol>'],

        ['<li>abc</li>', '<dir><li>f[o]o</dir>'],
        ['<li>abc</li>', '<ol><li>f[o]o</ol>'],
        ['<li>abc</li>', '<ul><li>f[o]o</ul>'],
        ['<dir><li>abc</dir>', '<dir><li>f[o]o</dir>'],
        ['<dir><li>abc</dir>', '<ol><li>f[o]o</ol>'],
        ['<dir><li>abc</dir>', '<ul><li>f[o]o</ul>'],
        ['<ol><li>abc</ol>', '<dir><li>f[o]o</dir>'],
        ['<ol><li>abc</ol>', '<ol><li>f[o]o</ol>'],
        ['<ol><li>abc</ol>', '<ul><li>f[o]o</ul>'],
        ['<ul><li>abc</ul>', '<dir><li>f[o]o</dir>'],
        ['<ul><li>abc</ul>', '<ol><li>f[o]o</ol>'],
        ['<ul><li>abc</ul>', '<ul><li>f[o]o</ul>'],
        ['<li>abc</li>', 'f[o]o'],

        ['<nobr>abc</nobr>', '<nobr>f[o]o</nobr>'],
        ['<nobr>abc</nobr>', 'f[o]o'],

        ['<p>abc', '<font color=blue>foo[]bar</font>'],
        ['<p>abc', '<span style=color:blue>foo[]bar</span>'],
        ['<p>abc', '<span style=font-variant:small-caps>foo[]bar</span>'],
        [' ', '<p>[foo]</p>'],
        ['<span style=display:none></span>', '<p>[foo]</p>'],
        ['<!--abc-->', '<p>[foo]</p>'],

        ['abc', '<p>{}<br></p>'],
        ['<!--abc-->', '<p>{}<br></p>'],
        ['abc', '<p><!--foo-->{}<span><br></span><!--bar--></p>'],
        ['<!--abc-->', '<p><!--foo-->{}<span><br></span><!--bar--></p>'],
        ['abc', '<p>{}<span><!--foo--><br><!--bar--></span></p>'],
        ['<!--abc-->', '<p>{}<span><!--foo--><br><!--bar--></span></p>'],

        ['abc', '<p><br>{}</p>'],
        ['<!--abc-->', '<p><br>{}</p>'],
        ['abc', '<p><!--foo--><span><br></span>{}<!--bar--></p>'],
        ['<!--abc-->', '<p><!--foo--><span><br></span>{}<!--bar--></p>'],
        ['abc', '<p><span><!--foo--><br><!--bar--></span>{}</p>'],
        ['<!--abc-->', '<p><span><!--foo--><br><!--bar--></span>{}</p>'],
    ],
    //@}
    insertimage: [
    //@{
        'foo[]bar',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        ["", 'foo[bar]baz'],
        'foo[bar]baz',
        'foo<span style=color:#aBcDeF>[bar]</span>baz',
        'foo<span style=color:#aBcDeF>{bar}</span>baz',
        'foo{<span style=color:#aBcDeF>bar</span>}baz',
        '[foo<span style=color:#aBcDeF>bar]</span>baz',
        '{foo<span style=color:#aBcDeF>bar}</span>baz',
        'foo<span style=color:#aBcDeF>[bar</span>baz]',
        'foo<span style=color:#aBcDeF>{bar</span>baz}',
        'foo<span style=color:#aBcDeF>[bar</span><span style=color:#fEdCbA>baz]</span>quz',

        'foo<b>[bar]</b>baz',
        'foo<b>{bar}</b>baz',
        'foo{<b>bar</b>}baz',
        'foo<span>[bar]</span>baz',
        'foo<span>{bar}</span>baz',
        'foo{<span>bar</span>}baz',
        '<b>foo[bar</b><i>baz]quz</i>',
        '<p>foo</p><p>[bar]</p><p>baz</p>',
        '<p>foo</p><p>{bar}</p><p>baz</p>',
        '<p>foo</p>{<p>bar</p>}<p>baz</p>',

        '<p>foo[bar<p>baz]quz',
        '<p>foo[bar<div>baz]quz</div>',
        '<p>foo[bar<h1>baz]quz</h1>',
        '<div>foo[bar</div><p>baz]quz',
        '<blockquote>foo[bar</blockquote><pre>baz]quz</pre>',

        '<p><b>foo[bar</b><p>baz]quz',
        '<div><p>foo[bar</div><p>baz]quz',
        '<p>foo[bar<blockquote><p>baz]quz<p>qoz</blockquote',
        '<p>foo[bar<p style=color:blue>baz]quz',
        '<p>foo[bar<p><b>baz]quz</b>',

        '<div><p>foo<p>[bar<p>baz]</div>',

        'foo[<br>]bar',
        '<p>foo[</p><p>]bar</p>',
        '<p>foo[</p><p>]bar<br>baz</p>',
        'foo[<p>]bar</p>',
        'foo[<p>]bar<br>baz</p>',
        'foo[<p>]bar</p>baz',
        '<p>foo[</p>]bar',
        '<p>foo[</p>]bar<br>baz',
        '<p>foo[</p>]bar<p>baz</p>',
        'foo[<div><p>]bar</div>',
        '<div><p>foo[</p></div>]bar',
        'foo[<div><p>]bar</p>baz</div>',
        'foo[<div>]bar<p>baz</p></div>',
        '<div><p>foo</p>bar[</div>]baz',
        '<div>foo<p>bar[</p></div>]baz',
    ],
    //@}
    insertlinebreak: [
    //@{ Same as insertparagraph (set below)
    ],
    //@}
    insertorderedlist: [
    //@{
        'foo[]bar',
        'foo[bar]baz',
        'foo<br>[bar]',
        'f[oo<br>b]ar<br>baz',
        '<p>[foo]<br>bar</p>',
        '[foo<ol><li>bar]</ol>baz',
        'foo<ol><li>[bar</ol>baz]',
        '[foo<ul><li>bar]</ul>baz',
        'foo<ul><li>[bar</ul>baz]',
        'foo<ul><li>[bar</ul><ol><li>baz]</ol>quz',
        'foo<ol><li>[bar</ol><ul><li>baz]</ul>quz',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr><td>fo[o<td>b]ar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        '<p>foo<p>[bar]<p>baz',
        '<p>foo<blockquote>[bar]</blockquote><p>baz',
        '<dl><dt>foo<dd>[bar]<dt>baz<dd>quz</dl>',
        '<dl><dt>foo<dd>bar<dt>[baz]<dd>quz</dl>',

        '<p>[foo<p>bar]<p>baz',
        '<p>[foo<blockquote>bar]</blockquote><p>baz',
        '<dl><dt>[foo<dd>bar]<dt>baz<dd>quz</dl>',
        '<dl><dt>foo<dd>[bar<dt>baz]<dd>quz</dl>',

        '<p>[foo<blockquote><p>bar]<p>baz</blockquote>',


        // Various <ol> stuff
        '<ol><li>foo<li>[bar]<li>baz</ol>',
        '<ol><li>foo</ol>[bar]',
        '[foo]<ol><li>bar</ol>',
        '<ol><li>foo</ol>[bar]<ol><li>baz</ol>',
        '<ol><ol><li>[foo]</ol></ol>',
        '<ol><li>[foo]<br>bar<li>baz</ol>',
        '<ol><li>foo<br>[bar]<li>baz</ol>',
        '<ol><li><div>[foo]</div>bar<li>baz</ol>',
        '<ol><li>foo<ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>[foo]<ol><li>bar</ol><li>baz</ol>',
        '<ol><li>[foo]</li><ol><li>bar</ol><li>baz</ol>',
        '<ol><li>foo<li>[bar]<ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<li>[bar]</li><ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>baz</ol><li>[quz]</ol>',
        '<ol><li>foo</li><ol><li>bar<li>baz</ol><li>[quz]</ol>',

        // Multiple items at once.
        '<ol><li>foo<li>[bar<li>baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol><li>baz</ol>',
        '<ol><li>foo<ol><li>b[ar</ol><li>b]az</ol>',
        '<ol><li>[foo<ol><li>bar</ol><li>baz]</ol><p>extra',

        // We probably can't actually get this DOM . . .
        '<ol><li>[foo]<ol><li>bar</ol>baz</ol>',
        '<ol><li>foo<ol><li>[bar]</ol>baz</ol>',
        '<ol><li>foo<ol><li>bar</ol>[baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol>baz</ol>',


        // Same stuff but with <ul>
        '<ul><li>foo<li>[bar]<li>baz</ul>',
        '<ul><li>foo</ul>[bar]',
        '[foo]<ul><li>bar</ul>',
        '<ul><li>foo</ul>[bar]<ul><li>baz</ul>',
        '<ul><ul><li>[foo]</ul></ul>',
        '<ul><li>[foo]<br>bar<li>baz</ul>',
        '<ul><li>foo<br>[bar]<li>baz</ul>',
        '<ul><li><div>[foo]</div>bar<li>baz</ul>',
        '<ul><li>foo<ul><li>[bar]<li>baz</ul><li>quz</ul>',
        '<ul><li>foo<ul><li>bar<li>[baz]</ul><li>quz</ul>',
        '<ul><li>foo</li><ul><li>[bar]<li>baz</ul><li>quz</ul>',
        '<ul><li>foo</li><ul><li>bar<li>[baz]</ul><li>quz</ul>',
        '<ul><li>[foo]<ul><li>bar</ul><li>baz</ul>',
        '<ul><li>[foo]</li><ul><li>bar</ul><li>baz</ul>',
        '<ul><li>foo<li>[bar]<ul><li>baz</ul><li>quz</ul>',
        '<ul><li>foo<li>[bar]</li><ul><li>baz</ul><li>quz</ul>',
        '<ul><li>foo<ul><li>bar<li>baz</ul><li>[quz]</ul>',
        '<ul><li>foo</li><ul><li>bar<li>baz</ul><li>[quz]</ul>',

        // Multiple items at once.
        '<ul><li>foo<li>[bar<li>baz]</ul>',
        '<ul><li>[foo<ul><li>bar]</ul><li>baz</ul>',
        '<ul><li>foo<ul><li>b[ar</ul><li>b]az</ul>',
        '<ul><li>[foo<ul><li>bar</ul><li>baz]</ul><p>extra',

        // We probably can't actually get this DOM . . .
        '<ul><li>[foo]<ul><li>bar</ul>baz</ul>',
        '<ul><li>foo<ul><li>[bar]</ul>baz</ul>',
        '<ul><li>foo<ul><li>bar</ul>[baz]</ul>',
        '<ul><li>[foo<ul><li>bar]</ul>baz</ul>',


        // Mix of <ol> and <ul>
        'foo<ol><li>bar</ol><ul><li>[baz]</ul>quz',
        'foo<ol><li>bar</ol><ul><li>[baz</ul>quz]',
        'foo<ul><li>[bar]</ul><ol><li>baz</ol>quz',
        '[foo<ul><li>bar]</ul><ol><li>baz</ol>quz',

        // Interaction with indentation
        '[foo]<blockquote>bar</blockquote>baz',
        'foo<blockquote>[bar]</blockquote>baz',
        '[foo<blockquote>bar]</blockquote>baz',
        '<ol><li>foo</ol><blockquote>[bar]</blockquote>baz',
        '[foo]<blockquote><ol><li>bar</ol></blockquote>baz',
        'foo<blockquote>[bar]<br>baz</blockquote>',
        '[foo<blockquote>bar]<br>baz</blockquote>',
        '<ol><li>foo</ol><blockquote>[bar]<br>baz</blockquote>',

        '<p>[foo]<blockquote><p>bar</blockquote><p>baz',
        '<p>foo<blockquote><p>[bar]</blockquote><p>baz',
        '<p>[foo<blockquote><p>bar]</blockquote><p>baz',
        '<ol><li>foo</ol><blockquote><p>[bar]</blockquote><p>baz',

        // Attributes
        '<ul id=abc><li>foo<li>[bar]<li>baz</ul>',
        '<ul style=color:blue><li>foo<li>[bar]<li>baz</ul>',
        '<ul style=text-indent:1em><li>foo<li>[bar]<li>baz</ul>',
        '<ul id=abc><li>[foo]<li>bar<li>baz</ul>',
        '<ul style=color:blue><li>[foo]<li>bar<li>baz</ul>',
        '<ul style=text-indent:1em><li>[foo]<li>bar<li>baz</ul>',
        '<ul id=abc><li>foo<li>bar<li>[baz]</ul>',
        '<ul style=color:blue><li>foo<li>bar<li>[baz]</ul>',
        '<ul style=text-indent:1em><li>foo<li>bar<li>[baz]</ul>',

        // Whitespace nodes
        '<ol><li>foo</ol> <p>[bar]',
        '<p>[foo]</p> <ol><li>bar</ol>',
        '<ol><li>foo</ol> <p>[bar]</p> <ol><li>baz</ol>',

        // This caused an infinite loop at one point due to a bug in "fix
        // disallowed ancestors".  Disabled because I'm not sure how we want it
        // to behave:
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=14578
        '!<span contenteditable=true>foo[]</span>',
    ],
    //@}
    insertparagraph: [
    //@{
        'foo[bar]baz',
        'fo[o<table><tr><td>b]ar</table>',
        '<table><tr><td>[foo<td>bar]<tr><td>baz<td>quz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<tr><td>baz<td>quz</table>',
        '<table><tr><td>fo[o</table>b]ar',
        '<table><tr><td>fo[o<td>b]ar<td>baz</table>',
        '{<table><tr><td>foo</table>}',
        '<table><tr><td>[foo]</table>',
        '<ol><li>[foo]<li>bar</ol>',
        '<ol><li>f[o]o<li>bar</ol>',

        '[]foo',
        'foo[]',
        '<span>foo[]</span>',
        'foo[]<br>',
        'foo[]bar',
        '<address>[]foo</address>',
        '<address>foo[]</address>',
        '<address>foo[]<br></address>',
        '<address>foo[]bar</address>',
        '<div>[]foo</div>',
        '<div>foo[]</div>',
        '<div>foo[]<br></div>',
        '<div>foo[]bar</div>',
        '<dl><dt>[]foo<dd>bar</dl>',
        '<dl><dt>foo[]<dd>bar</dl>',
        '<dl><dt>foo[]<br><dd>bar</dl>',
        '<dl><dt>foo[]bar<dd>baz</dl>',
        '<dl><dt>foo<dd>[]bar</dl>',
        '<dl><dt>foo<dd>bar[]</dl>',
        '<dl><dt>foo<dd>bar[]<br></dl>',
        '<dl><dt>foo<dd>bar[]baz</dl>',
        '<h1>[]foo</h1>',
        '<h1>foo[]</h1>',
        '<h1>foo[]<br></h1>',
        '<h1>foo[]bar</h1>',
        '<ol><li>[]foo</ol>',
        '<ol><li>foo[]</ol>',
        '<ol><li>foo[]<br></ol>',
        '<ol><li>foo[]bar</ol>',
        '<p>[]foo</p>',
        '<p>foo[]</p>',
        '<p>foo[]<br></p>',
        '<p>foo[]bar</p>',
        '<pre>[]foo</pre>',
        '<pre>foo[]</pre>',
        '<pre>foo[]<br></pre>',
        '<pre>foo[]bar</pre>',

        '<pre>foo[]<br><br></pre>',
        '<pre>foo<br>{}<br></pre>',
        '<pre>foo&#10;[]</pre>',
        '<pre>foo[]&#10;</pre>',
        '<pre>foo&#10;[]&#10;</pre>',

        '<xmp>foo[]bar</xmp>',
        '<script>foo[]bar</script>baz',
        '<div style=display:none>foo[]bar</div>baz',
        '<listing>foo[]bar</listing>',

        '<ol><li>{}<br></li></ol>',
        'foo<ol><li>{}<br></li></ol>',
        '<ol><li>{}<br></li></ol>foo',
        '<ol><li>foo<li>{}<br></ol>',
        '<ol><li>{}<br><li>bar</ol>',
        '<ol><li>foo</li><ul><li>{}<br></ul></ol>',

        '<dl><dt>{}<br></dt></dl>',
        '<dl><dt>foo<dd>{}<br></dl>',
        '<dl><dt>{}<br><dd>bar</dl>',
        '<dl><dt>foo<dd>bar<dl><dt>{}<br><dd>baz</dl></dl>',
        '<dl><dt>foo<dd>bar<dl><dt>baz<dd>{}<br></dl></dl>',

        '<h1>foo[bar</h1><p>baz]quz</p>',
        '<p>foo[bar</p><h1>baz]quz</h1>',
        '<p>foo</p>{}<br>',
        '{}<br><p>foo</p>',
        '<p>foo</p>{}<br><h1>bar</h1>',
        '<h1>foo</h1>{}<br><p>bar</p>',
        '<h1>foo</h1>{}<br><h2>bar</h2>',
        '<p>foo</p><h1>[bar]</h1><p>baz</p>',
        '<p>foo</p>{<h1>bar</h1>}<p>baz</p>',

        '<table><tr><td>foo[]bar</table>',
        '<table><tr><td><p>foo[]bar</table>',

        '<blockquote>[]foo</blockquote>',
        '<blockquote>foo[]</blockquote>',
        '<blockquote>foo[]<br></blockquote>',
        '<blockquote>foo[]bar</blockquote>',
        '<blockquote><p>[]foo</blockquote>',
        '<blockquote><p>foo[]</blockquote>',
        '<blockquote><p>foo[]bar</blockquote>',
        '<blockquote><p>foo[]<p>bar</blockquote>',
        '<blockquote><p>foo[]bar<p>baz</blockquote>',

        '<span>foo[]bar</span>',
        '<span>foo[]bar</span>baz',
        '<b>foo[]bar</b>',
        '<b>foo[]bar</b>baz',
        '<b>foo[]</b>bar',
        'foo<b>[]bar</b>',
        '<b>foo[]</b><i>bar</i>',
        '<b id=x class=y>foo[]bar</b>',
        '<i><b>foo[]bar</b>baz</i>',

        '<p><b>foo[]bar</b></p>',
        '<p><b>[]foo</b></p>',
        '<p><b id=x class=y>foo[]bar</b></p>',
        '<div><b>foo[]bar</b></div>',

        '<a href=foo>foo[]bar</a>',
        '<a href=foo>foo[]bar</a>baz',
        '<a href=foo>foo[]</a>bar',
        'foo<a href=foo>[]bar</a>',

        '<p>foo[]<!--bar-->',
        '<p><!--foo-->[]bar',

        '<p>foo<span style=color:#aBcDeF>[bar]</span>baz',
        '<p>foo<span style=color:#aBcDeF>{bar}</span>baz',
        '<p>foo{<span style=color:#aBcDeF>bar</span>}baz',
        '<p>[foo<span style=color:#aBcDeF>bar]</span>baz',
        '<p>{foo<span style=color:#aBcDeF>bar}</span>baz',
        '<p>foo<span style=color:#aBcDeF>[bar</span>baz]',
        '<p>foo<span style=color:#aBcDeF>{bar</span>baz}',
        '<p>foo<span style=color:#aBcDeF>[bar</span><span style=color:#fEdCbA>baz]</span>quz',

        // https://bugs.webkit.org/show_bug.cgi?id=5036
        '<ul contenteditable><li>{}<br></ul>',
        '<ul contenteditable><li>foo[]</ul>',
        '<div contenteditable=false><ul contenteditable><li>{}<br></ul></div>',
        '<div contenteditable=false><ul contenteditable><li>foo[]</ul></div>',

        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=13841
        // https://bugs.webkit.org/show_bug.cgi?id=23507
        '<address><p>foo[]</address>',
        '<dl><dt><p>foo[]</dl>',
        '<dl><dd><p>foo[]</dl>',
        '<ol><li><p>foo[]</ol>',
        '<ul><li><p>foo[]</ul>',
        '<address><div>foo[]</address>',
        '<dl><dt><div>foo[]</dl>',
        '<dl><dd><div>foo[]</dl>',
        '<ol><li><div>foo[]</ol>',
        '<ul><li><div>foo[]</ul>',
        '<div><p>foo[]</div>',
        '<div><div>foo[]</div>',

        '<address><p>[]foo</address>',
        '<dl><dt><p>[]foo</dl>',
        '<dl><dd><p>[]foo</dl>',
        '<ol><li><p>[]foo</ol>',
        '<ul><li><p>[]foo</ul>',
        '<address><div>[]foo</address>',
        '<dl><dt><div>[]foo</dl>',
        '<dl><dd><div>[]foo</dl>',
        '<ol><li><div>[]foo</ol>',
        '<ul><li><div>[]foo</ul>',
        '<div><p>[]foo</div>',
        '<div><div>[]foo</div>',

        '<address><p>foo[]bar</address>',
        '<dl><dt><p>foo[]bar</dl>',
        '<dl><dd><p>foo[]bar</dl>',
        '<ol><li><p>foo[]bar</ol>',
        '<ul><li><p>foo[]bar</ul>',
        '<address><div>foo[]bar</address>',
        '<dl><dt><div>foo[]bar</dl>',
        '<dl><dd><div>foo[]bar</dl>',
        '<ol><li><div>foo[]bar</ol>',
        '<ul><li><div>foo[]bar</ul>',
        '<div><p>foo[]bar</div>',
        '<div><div>foo[]bar</div>',

        '<ol><li class=a id=x><p class=b id=y>foo[]</ol>',
        '<div class=a id=x><div class=b id=y>foo[]</div></div>',
        '<div class=a id=x><p class=b id=y>foo[]</div>',
        '<ol><li class=a id=x><p class=b id=y>[]foo</ol>',
        '<div class=a id=x><div class=b id=y>[]foo</div></div>',
        '<div class=a id=x><p class=b id=y>[]foo</div>',
        '<ol><li class=a id=x><p class=b id=y>foo[]bar</ol>',
        '<div class=a id=x><div class=b id=y>foo[]bar</div></div>',
        '<div class=a id=x><p class=b id=y>foo[]bar</div>',
    ],
    //@}
    inserttext: [
    //@{
        'foo[bar]baz',
        ['', 'foo[bar]baz'],

        ['\t', 'foo[]bar'],
        ['&', 'foo[]bar'],
        ['\n', 'foo[]bar'],
        ['abc\ndef', 'foo[]bar'],
        ['\x07', 'foo[]bar'],

        ['<b>hi</b>', 'foo[]bar'],
        ['<', 'foo[]bar'],
        ['&amp;', 'foo[]bar'],

        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=14254
        ['!\r', 'foo[]bar'],
        ['!\r\n', 'foo[]bar'],
        ['!\0', 'foo[]bar'],
        ['!\ud800', 'foo[]bar'],

        // Whitespace tests!  The following two bugs are relevant to some of
        // these:
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=14119
        // https://bugzilla.mozilla.org/show_bug.cgi?id=681626
        [' ', 'foo[]bar'],
        [' ', 'foo []bar'],
        [' ', 'foo[] bar'],
        [' ', 'foo &nbsp;[]bar'],
        [' ', 'foo []&nbsp;bar'],
        [' ', 'foo[] &nbsp;bar'],
        [' ', 'foo&nbsp; []bar'],
        [' ', 'foo&nbsp;[] bar'],
        [' ', 'foo[]&nbsp; bar'],
        [' ', 'foo&nbsp;&nbsp;[]bar'],
        [' ', 'foo&nbsp;[]&nbsp;bar'],
        [' ', 'foo[]&nbsp;&nbsp;bar'],
        [' ', 'foo []&nbsp;        bar'],
        [' ', 'foo  []bar'],
        [' ', 'foo []&nbsp;&nbsp; &nbsp; bar'],

        [' ', '[]foo'],
        [' ', '{}foo'],
        [' ', 'foo[]'],
        [' ', 'foo{}'],
        [' ', 'foo&nbsp;[]'],
        [' ', 'foo&nbsp;{}'],
        [' ', 'foo&nbsp;&nbsp;[]'],
        [' ', 'foo&nbsp;&nbsp;{}'],
        [' ', '<b>foo[]</b>bar'],
        [' ', 'foo[]<b>bar</b>'],

        [' ', 'foo[] '],
        [' ', ' foo   []   '],
        [' ', 'foo[]<span> </span>'],
        [' ', 'foo[]<span> </span> '],
        [' ', ' []foo'],
        [' ', '   []   foo '],
        [' ', '<span> </span>[]foo'],
        [' ', ' <span> </span>[]foo'],

        [' ', '{}<br>'],
        [' ', '<p>{}<br>'],

        [' ', '<p>foo[]<p>bar'],
        [' ', '<p>foo&nbsp;[]<p>bar'],
        [' ', '<p>foo[]<p>&nbsp;bar'],

        // Some of the same tests as above, repeated with various values of
        // white-space.
        [' ', '<pre>foo[]bar</pre>'],
        [' ', '<pre>foo []bar</pre>'],
        [' ', '<pre>foo[] bar</pre>'],
        [' ', '<pre>foo &nbsp;[]bar</pre>'],
        [' ', '<pre>[]foo</pre>'],
        [' ', '<pre>foo[]</pre>'],
        [' ', '<pre>foo&nbsp;[]</pre>'],
        [' ', '<pre> foo   []   </pre>'],

        [' ', '<div style=white-space:pre>foo[]bar</div>'],
        [' ', '<div style=white-space:pre>foo []bar</div>'],
        [' ', '<div style=white-space:pre>foo[] bar</div>'],
        [' ', '<div style=white-space:pre>foo &nbsp;[]bar</div>'],
        [' ', '<div style=white-space:pre>[]foo</div>'],
        [' ', '<div style=white-space:pre>foo[]</div>'],
        [' ', '<div style=white-space:pre>foo&nbsp;[]</div>'],
        [' ', '<div style=white-space:pre> foo   []   </div>'],

        [' ', '<div style=white-space:pre-wrap>foo[]bar</div>'],
        [' ', '<div style=white-space:pre-wrap>foo []bar</div>'],
        [' ', '<div style=white-space:pre-wrap>foo[] bar</div>'],
        [' ', '<div style=white-space:pre-wrap>foo &nbsp;[]bar</div>'],
        [' ', '<div style=white-space:pre-wrap>[]foo</div>'],
        [' ', '<div style=white-space:pre-wrap>foo[]</div>'],
        [' ', '<div style=white-space:pre-wrap>foo&nbsp;[]</div>'],
        [' ', '<div style=white-space:pre-wrap> foo   []   </div>'],

        [' ', '<div style=white-space:pre-line>foo[]bar</div>'],
        [' ', '<div style=white-space:pre-line>foo []bar</div>'],
        [' ', '<div style=white-space:pre-line>foo[] bar</div>'],
        [' ', '<div style=white-space:pre-line>foo &nbsp;[]bar</div>'],
        [' ', '<div style=white-space:pre-line>[]foo</div>'],
        [' ', '<div style=white-space:pre-line>foo[]</div>'],
        [' ', '<div style=white-space:pre-line>foo&nbsp;[]</div>'],
        [' ', '<div style=white-space:pre-line> foo   []   </div>'],

        [' ', '<div style=white-space:nowrap>foo[]bar</div>'],
        [' ', '<div style=white-space:nowrap>foo []bar</div>'],
        [' ', '<div style=white-space:nowrap>foo[] bar</div>'],
        [' ', '<div style=white-space:nowrap>foo &nbsp;[]bar</div>'],
        [' ', '<div style=white-space:nowrap>[]foo</div>'],
        [' ', '<div style=white-space:nowrap>foo[]</div>'],
        [' ', '<div style=white-space:nowrap>foo&nbsp;[]</div>'],
        [' ', '<div style=white-space:nowrap> foo   []   </div>'],

        // End whitespace tests

        // Autolinking tests
        [' ', 'http://a[]'],
        [' ', 'ftp://a[]'],
        [' ', 'quasit://a[]'],
        [' ', '.x-++-.://a[]'],
        [' ', '(http://a)[]'],
        [' ', '&lt;http://a>[]'],
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=14744
        ['! ', '&#x5b;http://a&#x5d;[]'],
        ['! ', '&#x7b;http://a&#x7d;[]'],
        [' ', 'http://a![]'],
        [' ', '!"#$%&amp;\'()*+,-./:;&lt;=>?\^_`|~http://a!"#$%&amp;\'()*+,-./:;&lt;=>?\^_`|~[]'],
        [' ', 'http://a!"\'(),-.:;&lt;>`[]'],
        [' ', 'http://a#$%&amp;*+/=?\^_|~[]'],
        [' ', 'mailto:a[]'],
        [' ', 'a@b[]'],
        [' ', 'a@[]'],
        [' ', '@b[]'],
        [' ', '#@x[]'],
        [' ', 'a@.[]'],
        [' ', '!"#$%&amp;\'()*+,-./:;&lt;=>?\^_`|~a@b!"#$%&amp;\'()*+,-./:;&lt;=>?\^_`|~[]'],
        [' ', '<b>a@b</b>{}'],
        [' ', '<b>a</b><i>@</i><u>b</u>{}'],
        [' ', 'a@b<b>[]c</b>'],
        [' ', '<p>a@b</p><p>[]c</p>'],
        ['a', 'http://a[]'],
        ['\t', 'http://a[]'],
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=14254
        ['!\r', 'http://a[]'],
        // http://www.w3.org/Bugs/Public/show_bug.cgi?id=14745
        ['!\n', 'http://a[]'],
        ['\f', 'http://a[]'],
        ['\u00A0', 'http://a[]'],

        ['   ', 'foo[]'],

        'foo[]bar',
        'foo&nbsp;[]',
        'foo\xa0[]',
        '<p>foo[]',
        '<p>foo</p>{}',
        '<p>[]foo',
        '<p>{}foo',
        '{}<p>foo',
        '<p>foo</p>{}<p>bar</p>',
        '<b>foo[]</b>bar',
        '<b>foo</b>[]bar',
        'foo<b>{}</b>bar',
        '<a>foo[]</a>bar',
        '<a>foo</a>[]bar',
        '<a href=/>foo[]</a>bar',
        '<a href=/>foo</a>[]bar',
        '<p>fo[o<p>b]ar',
        '<p>fo[o<p>bar<p>b]az',
        '{}<br>',
        '<p>{}<br>',
        '<p><span>{}<br></span>',
        '<p>foo<span style=color:#aBcDeF>[bar]</span>baz',
        '<p>foo<span style=color:#aBcDeF>{bar}</span>baz',
        '<p>foo{<span style=color:#aBcDeF>bar</span>}baz',
        '<p>[foo<span style=color:#aBcDeF>bar]</span>baz',
        '<p>{foo<span style=color:#aBcDeF>bar}</span>baz',
        '<p>foo<span style=color:#aBcDeF>[bar</span>baz]',
        '<p>foo<span style=color:#aBcDeF>{bar</span>baz}',
        '<p>foo<span style=color:#aBcDeF>[bar</span><span style=color:#fEdCbA>baz]</span>quz',


        // These are like the corresponding tests in the multitest section, but
        // because the selection isn't collapsed, we don't need to do
        // multitests to set overrides.
        'foo<b>[bar]</b>baz',
        'foo<i>[bar]</i>baz',
        'foo<s>[bar]</s>baz',
        'foo<sub>[bar]</sub>baz',
        'foo<sup>[bar]</sup>baz',
        'foo<u>[bar]</u>baz',
        'foo<a href=http://www.google.com>[bar]</a>baz',
        'foo<font face=sans-serif>[bar]</font>baz',
        'foo<font size=4>[bar]</font>baz',
        'foo<font color=#0000FF>[bar]</font>baz',
        'foo<span style=background-color:#00FFFF>[bar]</span>baz',
        'foo<a href=http://www.google.com><font color=blue>[bar]</font></a>baz',
        'foo<font color=blue><a href=http://www.google.com>[bar]</a></font>baz',
        'foo<a href=http://www.google.com><font color=brown>[bar]</font></a>baz',
        'foo<font color=brown><a href=http://www.google.com>[bar]</a></font>baz',
        'foo<a href=http://www.google.com><font color=black>[bar]</font></a>baz',
        'foo<a href=http://www.google.com><u>[bar]</u></a>baz',
        'foo<u><a href=http://www.google.com>[bar]</a></u>baz',
        'foo<sub><font size=2>[bar]</font></sub>baz',
        'foo<font size=2><sub>[bar]</sub></font>baz',
        'foo<sub><font size=3>[bar]</font></sub>baz',
        'foo<font size=3><sub>[bar]</sub></font>baz',

        // Now repeat but with different selections.
        '[foo<b>bar]</b>baz',
        '[foo<i>bar]</i>baz',
        '[foo<s>bar]</s>baz',
        '[foo<sub>bar]</sub>baz',
        '[foo<sup>bar]</sup>baz',
        '[foo<u>bar]</u>baz',
        '[foo<a href=http://www.google.com>bar]</a>baz',
        '[foo<font face=sans-serif>bar]</font>baz',
        '[foo<font size=4>bar]</font>baz',
        '[foo<font color=#0000FF>bar]</font>baz',
        '[foo<span style=background-color:#00FFFF>bar]</span>baz',
        '[foo<a href=http://www.google.com><font color=blue>bar]</font></a>baz',
        '[foo<font color=blue><a href=http://www.google.com>bar]</a></font>baz',
        '[foo<a href=http://www.google.com><font color=brown>bar]</font></a>baz',
        '[foo<font color=brown><a href=http://www.google.com>bar]</a></font>baz',
        '[foo<a href=http://www.google.com><font color=black>bar]</font></a>baz',
        '[foo<a href=http://www.google.com><u>bar]</u></a>baz',
        '[foo<u><a href=http://www.google.com>bar]</a></u>baz',
        '[foo<sub><font size=2>bar]</font></sub>baz',
        '[foo<font size=2><sub>bar]</sub></font>baz',
        '[foo<sub><font size=3>bar]</font></sub>baz',
        '[foo<font size=3><sub>bar]</sub></font>baz',

        'foo<b>[bar</b>baz]',
        'foo<i>[bar</i>baz]',
        'foo<s>[bar</s>baz]',
        'foo<sub>[bar</sub>baz]',
        'foo<sup>[bar</sup>baz]',
        'foo<u>[bar</u>baz]',
        'foo<a href=http://www.google.com>[bar</a>baz]',
        'foo<font face=sans-serif>[bar</font>baz]',
        'foo<font size=4>[bar</font>baz]',
        'foo<font color=#0000FF>[bar</font>baz]',
        'foo<span style=background-color:#00FFFF>[bar</span>baz]',
        'foo<a href=http://www.google.com><font color=blue>[bar</font></a>baz]',
        'foo<font color=blue><a href=http://www.google.com>[bar</a></font>baz]',
        'foo<a href=http://www.google.com><font color=brown>[bar</font></a>baz]',
        'foo<font color=brown><a href=http://www.google.com>[bar</a></font>baz]',
        'foo<a href=http://www.google.com><font color=black>[bar</font></a>baz]',
        'foo<a href=http://www.google.com><u>[bar</u></a>baz]',
        'foo<u><a href=http://www.google.com>[bar</a></u>baz]',
        'foo<sub><font size=2>[bar</font></sub>baz]',
        'foo<font size=2><sub>[bar</sub></font>baz]',
        'foo<sub><font size=3>[bar</font></sub>baz]',
        'foo<font size=3><sub>[bar</sub></font>baz]',

        // https://bugs.webkit.org/show_bug.cgi?id=19702
        '<blockquote><font color=blue>[foo]</font></blockquote>',
    ],
    //@}
    insertunorderedlist: [
    //@{
        'foo[]bar',
        'foo[bar]baz',
        'foo<br>[bar]',
        'f[oo<br>b]ar<br>baz',
        '<p>[foo]<br>bar</p>',
        '[foo<ol><li>bar]</ol>baz',
        'foo<ol><li>[bar</ol>baz]',
        '[foo<ul><li>bar]</ul>baz',
        'foo<ul><li>[bar</ul>baz]',
        'foo<ul><li>[bar</ul><ol><li>baz]</ol>quz',
        'foo<ol><li>[bar</ol><ul><li>baz]</ul>quz',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr><td>fo[o<td>b]ar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        '<p>foo<p>[bar]<p>baz',
        '<p>foo<blockquote>[bar]</blockquote><p>baz',
        '<dl><dt>foo<dd>[bar]<dt>baz<dd>quz</dl>',
        '<dl><dt>foo<dd>bar<dt>[baz]<dd>quz</dl>',

        '<p>[foo<p>bar]<p>baz',
        '<p>[foo<blockquote>bar]</blockquote><p>baz',
        '<dl><dt>[foo<dd>bar]<dt>baz<dd>quz</dl>',
        '<dl><dt>foo<dd>[bar<dt>baz]<dd>quz</dl>',

        '<p>[foo<blockquote><p>bar]<p>baz</blockquote>',


        // Various <ol> stuff
        '<ol><li>foo<li>[bar]<li>baz</ol>',
        '<ol><li>foo</ol>[bar]',
        '[foo]<ol><li>bar</ol>',
        '<ol><li>foo</ol>[bar]<ol><li>baz</ol>',
        '<ol><ol><li>[foo]</ol></ol>',
        '<ol><li>[foo]<br>bar<li>baz</ol>',
        '<ol><li>foo<br>[bar]<li>baz</ol>',
        '<ol><li><div>[foo]</div>bar<li>baz</ol>',
        '<ol><li>foo<ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>[foo]<ol><li>bar</ol><li>baz</ol>',
        '<ol><li>[foo]</li><ol><li>bar</ol><li>baz</ol>',
        '<ol><li>foo<li>[bar]<ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<li>[bar]</li><ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>baz</ol><li>[quz]</ol>',
        '<ol><li>foo</li><ol><li>bar<li>baz</ol><li>[quz]</ol>',

        // Multiple items at once.
        '<ol><li>foo<li>[bar<li>baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol><li>baz</ol>',
        '<ol><li>foo<ol><li>b[ar</ol><li>b]az</ol>',
        '<ol><li>[foo<ol><li>bar</ol><li>baz]</ol><p>extra',

        // We probably can't actually get this DOM . . .
        '<ol><li>[foo]<ol><li>bar</ol>baz</ol>',
        '<ol><li>foo<ol><li>[bar]</ol>baz</ol>',
        '<ol><li>foo<ol><li>bar</ol>[baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol>baz</ol>',


        // Same stuff but with <ul>
        '<ul><li>foo<li>[bar]<li>baz</ul>',
        '<ul><li>foo</ul>[bar]',
        '[foo]<ul><li>bar</ul>',
        '<ul><li>foo</ul>[bar]<ul><li>baz</ul>',
        '<ul><ul><li>[foo]</ul></ul>',
        '<ul><li>[foo]<br>bar<li>baz</ul>',
        '<ul><li>foo<br>[bar]<li>baz</ul>',
        '<ul><li><div>[foo]</div>bar<li>baz</ul>',
        '<ul><li>foo<ul><li>[bar]<li>baz</ul><li>quz</ul>',
        '<ul><li>foo<ul><li>bar<li>[baz]</ul><li>quz</ul>',
        '<ul><li>foo</li><ul><li>[bar]<li>baz</ul><li>quz</ul>',
        '<ul><li>foo</li><ul><li>bar<li>[baz]</ul><li>quz</ul>',
        '<ul><li>[foo]<ul><li>bar</ul><li>baz</ul>',
        '<ul><li>[foo]</li><ul><li>bar</ul><li>baz</ul>',
        '<ul><li>foo<li>[bar]<ul><li>baz</ul><li>quz</ul>',
        '<ul><li>foo<li>[bar]</li><ul><li>baz</ul><li>quz</ul>',
        '<ul><li>foo<ul><li>bar<li>baz</ul><li>[quz]</ul>',
        '<ul><li>foo</li><ul><li>bar<li>baz</ul><li>[quz]</ul>',

        // Multiple items at once.
        '<ul><li>foo<li>[bar<li>baz]</ul>',
        '<ul><li>[foo<ul><li>bar]</ul><li>baz</ul>',
        '<ul><li>foo<ul><li>b[ar</ul><li>b]az</ul>',
        '<ul><li>[foo<ul><li>bar</ul><li>baz]</ul><p>extra',

        // We probably can't actually get this DOM . . .
        '<ul><li>[foo]<ul><li>bar</ul>baz</ul>',
        '<ul><li>foo<ul><li>[bar]</ul>baz</ul>',
        '<ul><li>foo<ul><li>bar</ul>[baz]</ul>',
        '<ul><li>[foo<ul><li>bar]</ul>baz</ul>',


        // Mix of <ol> and <ul>
        'foo<ol><li>bar</ol><ul><li>[baz]</ul>quz',
        'foo<ol><li>bar</ol><ul><li>[baz</ul>quz]',
        'foo<ul><li>[bar]</ul><ol><li>baz</ol>quz',
        '[foo<ul><li>bar]</ul><ol><li>baz</ol>quz',

        // Interaction with indentation
        '[foo]<blockquote>bar</blockquote>baz',
        'foo<blockquote>[bar]</blockquote>baz',
        '[foo<blockquote>bar]</blockquote>baz',
        '<ol><li>foo</ol><blockquote>[bar]</blockquote>baz',
        '[foo]<blockquote><ol><li>bar</ol></blockquote>baz',
        'foo<blockquote>[bar]<br>baz</blockquote>',
        '[foo<blockquote>bar]<br>baz</blockquote>',
        '<ol><li>foo</ol><blockquote>[bar]<br>baz</blockquote>',

        '<p>[foo]<blockquote><p>bar</blockquote><p>baz',
        '<p>foo<blockquote><p>[bar]</blockquote><p>baz',
        '<p>[foo<blockquote><p>bar]</blockquote><p>baz',
        '<ol><li>foo</ol><blockquote><p>[bar]</blockquote><p>baz',

        // Attributes
        '<ul id=abc><li>foo<li>[bar]<li>baz</ul>',
        '<ul style=color:blue><li>foo<li>[bar]<li>baz</ul>',
        '<ul style=text-indent:1em><li>foo<li>[bar]<li>baz</ul>',
        '<ul id=abc><li>[foo]<li>bar<li>baz</ul>',
        '<ul style=color:blue><li>[foo]<li>bar<li>baz</ul>',
        '<ul style=text-indent:1em><li>[foo]<li>bar<li>baz</ul>',
        '<ul id=abc><li>foo<li>bar<li>[baz]</ul>',
        '<ul style=color:blue><li>foo<li>bar<li>[baz]</ul>',
        '<ul style=text-indent:1em><li>foo<li>bar<li>[baz]</ul>',

        // Whitespace nodes
        '<ul><li>foo</ul> <p>[bar]',
        '<p>[foo]</p> <ul><li>bar</ul>',
        '<ul><li>foo</ul> <p>[bar]</p> <ul><li>baz</ul>',

        // https://bugs.webkit.org/show_bug.cgi?id=24167
        '{<div style="font-size: 1.3em">1</div><div style="font-size: 1.1em">2</div>}',
    ],
    //@}
    italic: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<span style="font-style: italic">[bar]</span>baz',
        'foo<address>[bar]</address>baz',
        'foo<cite>[bar]</cite>baz',
        'foo<dfn>[bar]</dfn>baz',
        'foo<em>[bar]</em>baz',
        'foo<i>[bar]</i>baz',
        'foo<var>[bar]</var>baz',

        'foo{<address>bar</address>}baz',
        'foo{<cite>bar</cite>}baz',
        'foo{<dfn>bar</dfn>}baz',
        'foo{<em>bar</em>}baz',
        'foo{<i>bar</i>}baz',
        'foo{<var>bar</var>}baz',

        'foo<address>b[a]r</address>baz',
        'foo<cite>b[a]r</cite>baz',
        'foo<dfn>b[a]r</dfn>baz',
        'foo<em>b[a]r</em>baz',
        'foo<i>b[a]r</i>baz',
        'foo<var>b[a]r</var>baz',

        'fo[o<address>bar</address>b]az',
        'fo[o<cite>bar</cite>b]az',
        'fo[o<dfn>bar</dfn>b]az',
        'fo[o<em>bar</em>b]az',
        'fo[o<i>bar</i>b]az',
        'fo[o<var>bar</var>b]az',

        'foo[<address>bar</address>baz]',
        'foo[<cite>bar</cite>baz]',
        'foo[<dfn>bar</dfn>baz]',
        'foo[<em>bar</em>baz]',
        'foo[<i>bar</i>baz]',
        'foo[<var>bar</var>baz]',

        '[foo<address>bar</address>]baz',
        '[foo<cite>bar</cite>]baz',
        '[foo<dfn>bar</dfn>]baz',
        '[foo<em>bar</em>]baz',
        '[foo<i>bar</i>]baz',
        '[foo<var>bar</var>]baz',

        'foo<span style="font-style: italic">[bar]</span>baz',
        'foo<span style="font-style: oblique">[bar]</span>baz',
        'foo<span style="font-style: oblique">b[a]r</span>baz',

        '<i>{<p>foo</p><p>bar</p>}<p>baz</p></i>',
        '<i><p>foo[<b>bar</b>}</p><p>baz</p></i>',
        'foo [bar <b>baz] qoz</b> quz sic',
        'foo bar <b>baz [qoz</b> quz] sic',
        'foo [bar <i>baz] qoz</i> quz sic',
        'foo bar <i>baz [qoz</i> quz] sic',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<i>b]ar</i>baz',
        'foo<i>ba[r</i>b]az',
        'fo[o<i>bar</i>b]az',
        'foo[<i>b]ar</i>baz',
        'foo<i>ba[r</i>]baz',
        'foo[<i>bar</i>]baz',
        'foo<i>[bar]</i>baz',
        'foo{<i>bar</i>}baz',
        'fo[o<span style=font-style:italic>b]ar</span>baz',
        'fo[o<span style=font-style:oblique>b]ar</span>baz',
        '<span style=font-style:italic>fo[o</span><span style=font-style:oblique>b]ar</span>',
        '<span style=font-style:oblique>fo[o</span><span style=font-style:italic>b]ar</span>',
        '<i>fo[o</i><address>b]ar</address>',
    ],
    //@}
    justifycenter: [
    //@{
        'foo[]bar<p>extra',
        '<span>foo</span>{}<span>bar</span><p>extra',
        '<span>foo[</span><span>]bar</span><p>extra',
        'foo[bar]baz<p>extra',
        'foo[bar<b>baz]qoz</b>quz<p>extra',
        '<p>foo[]bar<p>extra',
        '<p>foo[bar]baz<p>extra',
        '<h1>foo[bar]baz</h1><p>extra',
        '<pre>foo[bar]baz</pre><p>extra',
        '<xmp>foo[bar]baz</xmp><p>extra',
        '<center><p>[foo]<p>bar</center><p>extra',
        '<center><p>[foo<p>bar]</center><p>extra',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table align=center><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table align=center><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=center><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=center><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=center data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table align=center><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody align=center><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody align=center><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=center><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=center data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody align=center><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tbody align=center><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody><tr align=center><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr align=center data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr align=center data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr align=center><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr align=center><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr align=center><td>foo<td>bar<td>baz</table>}<p>extra',

        '<div align=center><p>[foo]<p>bar</div><p>extra',
        '<div align=center><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:center><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:center><p>[foo<p>bar]</div><p>extra',

        '<div align=justify><p>[foo]<p>bar</div><p>extra',
        '<div align=justify><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:justify><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:justify><p>[foo<p>bar]</div><p>extra',

        '<div align=left><p>[foo]<p>bar</div><p>extra',
        '<div align=left><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:left><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:left><p>[foo<p>bar]</div><p>extra',

        '<div align=right><p>[foo]<p>bar</div><p>extra',
        '<div align=right><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:right><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:right><p>[foo<p>bar]</div><p>extra',

        '<center>foo</center>[bar]<p>extra',
        '[foo]<center>bar</center><p>extra',
        '<center>foo</center>[bar]<center>baz</center><p>extra',
        '<div align=center>foo</div>[bar]<p>extra',
        '[foo]<div align=center>bar</div><p>extra',
        '<div align=center>foo</div>[bar]<div align=center>baz</div><p>extra',
        '<div align=center><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div align=center><p>bar</div><p>extra',
        '<div align=center><p>foo</div><p>[bar]<div align=center><p>baz</div><p>extra',
        '<div style=text-align:center>foo</div>[bar]<p>extra',
        '[foo]<div style=text-align:center>bar</div><p>extra',
        '<div style=text-align:center>foo</div>[bar]<div style=text-align:center>baz</div><p>extra',
        '<div style=text-align:center><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div style=text-align:center><p>bar</div><p>extra',
        '<div style=text-align:center><p>foo</div><p>[bar]<div style=text-align:center><p>baz</div><p>extra',
        '<p align=center>foo<p>[bar]<p>extra',
        '<p>[foo]<p align=center>bar<p>extra',
        '<p align=center>foo<p>[bar]<p align=center>baz<p>extra',

        '<center>[foo</center>bar]<p>extra',
        '<center>fo[o</center>b]ar<p>extra',
        '<div align=center>[foo</div>bar]<p>extra',
        '<div align=center>fo[o</div>b]ar<p>extra',
        '<div style=text-align:center>[foo</div>bar]<p>extra',
        '<div style=text-align:center>fo[o</div>b]ar<p>extra',
        '<span style=text-align:center>[foo]</span><p>extra',
        '<span style=text-align:center>f[o]o</span><p>extra',

        '<div style=text-align:center>[foo<div style=text-align:left contenteditable=false>bar</div>baz]</div><p>extra',

        '<div align=nonsense><p>[foo]</div><p>extra',
        '<div style=text-align:inherit><p>[foo]</div><p>extra',
        '<quasit align=right><p>[foo]</p></quasit><p>extra',

        '<div align=center>{<div align=left>foo</div>}</div>',
        '<div align=left>{<div align=center>foo</div>}</div>',
        '<div align=center>{<div align=left>foo</div>bar}</div>',
        '<div align=left>{<div align=center>foo</div>bar}</div>',
        '<div align=center>{<div align=left>foo</div><img src=/img/lion.svg>}</div>',
        '<div align=left>{<div align=center>foo</div><img src=/img/lion.svg>}</div>',
        '<div align=center>{<div align=left>foo</div><!-- bar -->}</div>',
        '<div align=left>{<div align=center>foo</div><!-- bar -->}</div>',

        '<div style=text-align:start>[foo]</div><p>extra',
        '<div style=text-align:end>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:start>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:end>[foo]</div><p>extra',

        // Whitespace nodes
        '<div style=text-align:center><p>foo</div> <p>[bar]',
        '<div align=center><p>foo</div> <p>[bar]',
        '<center><p>foo</center> <p>[bar]',
        '<p>[foo]</p> <div style=text-align:center><p>bar</div>',
        '<p>[foo]</p> <div align=center><p>bar</div>',
        '<p>[foo]</p> <center><p>bar</center>',
        '<div style=text-align:center><p>foo</div> <p>[bar]</p> <div style=text-align:center><p>baz</div>',
        '<div align=center><p>foo</div> <p>[bar]</p> <div align=center><p>baz</div>',
        '<center><p>foo</center> <p>[bar]</p> <center><p>baz</center>',
    ],
    //@}
    justifyfull: [
    //@{
        'foo[]bar<p>extra',
        '<span>foo</span>{}<span>bar</span><p>extra',
        '<span>foo[</span><span>]bar</span><p>extra',
        'foo[bar]baz<p>extra',
        'foo[bar<b>baz]qoz</b>quz<p>extra',
        '<p>foo[]bar<p>extra',
        '<p>foo[bar]baz<p>extra',
        '<h1>foo[bar]baz</h1><p>extra',
        '<pre>foo[bar]baz</pre><p>extra',
        '<xmp>foo[bar]baz</xmp><p>extra',
        '<center><p>[foo]<p>bar</center><p>extra',
        '<center><p>[foo<p>bar]</center><p>extra',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table align=justify><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table align=justify><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=justify><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=justify><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=justify data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table align=justify><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody align=justify><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody align=justify><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=justify><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=justify data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody align=justify><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tbody align=justify><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody><tr align=justify><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr align=justify data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr align=justify data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr align=justify><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr align=justify><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr align=justify><td>foo<td>bar<td>baz</table>}<p>extra',

        '<div align=center><p>[foo]<p>bar</div><p>extra',
        '<div align=center><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:center><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:center><p>[foo<p>bar]</div><p>extra',

        '<div align=justify><p>[foo]<p>bar</div><p>extra',
        '<div align=justify><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:justify><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:justify><p>[foo<p>bar]</div><p>extra',

        '<div align=left><p>[foo]<p>bar</div><p>extra',
        '<div align=left><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:left><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:left><p>[foo<p>bar]</div><p>extra',

        '<div align=right><p>[foo]<p>bar</div><p>extra',
        '<div align=right><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:right><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:right><p>[foo<p>bar]</div><p>extra',

        '<div align=justify>foo</div>[bar]<p>extra',
        '[foo]<div align=justify>bar</div><p>extra',
        '<div align=justify>foo</div>[bar]<div align=justify>baz</div><p>extra',
        '<div align=justify><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div align=justify><p>bar</div><p>extra',
        '<div align=justify><p>foo</div><p>[bar]<div align=justify><p>baz</div><p>extra',
        '<div style=text-align:justify>foo</div>[bar]<p>extra',
        '[foo]<div style=text-align:justify>bar</div><p>extra',
        '<div style=text-align:justify>foo</div>[bar]<div style=text-align:justify>baz</div><p>extra',
        '<div style=text-align:justify><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div style=text-align:justify><p>bar</div><p>extra',
        '<div style=text-align:justify><p>foo</div><p>[bar]<div style=text-align:justify><p>baz</div><p>extra',
        '<p align=justify>foo<p>[bar]<p>extra',
        '<p>[foo]<p align=justify>bar<p>extra',
        '<p align=justify>foo<p>[bar]<p align=justify>baz<p>extra',

        '<div align=justify>[foo</div>bar]<p>extra',
        '<div align=justify>fo[o</div>b]ar<p>extra',
        '<div style=text-align:justify>[foo</div>bar]<p>extra',
        '<div style=text-align:justify>fo[o</div>b]ar<p>extra',
        '<span style=text-align:justify>[foo]</span><p>extra',
        '<span style=text-align:justify>f[o]o</span><p>extra',

        '<div style=text-align:justify>[foo<div style=text-align:left contenteditable=false>bar</div>baz]</div><p>extra',

        '<div align=nonsense><p>[foo]</div><p>extra',
        '<div style=text-align:inherit><p>[foo]</div><p>extra',
        '<quasit align=center><p>[foo]</p></quasit><p>extra',

        '<div style=text-align:start>[foo]</div><p>extra',
        '<div style=text-align:end>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:start>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:end>[foo]</div><p>extra',

        // Whitespace nodes
        '<div style=text-align:justify><p>foo</div> <p>[bar]',
        '<div align=justify><p>foo</div> <p>[bar]',
        '<p>[foo]</p> <div style=text-align:justify><p>bar</div>',
        '<p>[foo]</p> <div align=justify><p>bar</div>',
        '<div style=text-align:justify><p>foo</div> <p>[bar]</p> <div style=text-align:justify><p>baz</div>',
        '<div align=justify><p>foo</div> <p>[bar]</p> <div align=justify><p>baz</div>',
    ],
    //@}
    justifyleft: [
    //@{
        'foo[]bar<p>extra',
        '<span>foo</span>{}<span>bar</span><p>extra',
        '<span>foo[</span><span>]bar</span><p>extra',
        'foo[bar]baz<p>extra',
        'foo[bar<b>baz]qoz</b>quz<p>extra',
        '<p>foo[]bar<p>extra',
        '<p>foo[bar]baz<p>extra',
        '<h1>foo[bar]baz</h1><p>extra',
        '<pre>foo[bar]baz</pre><p>extra',
        '<xmp>foo[bar]baz</xmp><p>extra',
        '<center><p>[foo]<p>bar</center><p>extra',
        '<center><p>[foo<p>bar]</center><p>extra',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table align=left><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table align=left><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=left><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=left><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=left data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table align=left><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody align=left><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody align=left><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=left><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=left data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody align=left><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tbody align=left><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody><tr align=left><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr align=left data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr align=left data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr align=left><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr align=left><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr align=left><td>foo<td>bar<td>baz</table>}<p>extra',

        '<div align=center><p>[foo]<p>bar</div><p>extra',
        '<div align=center><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:center><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:center><p>[foo<p>bar]</div><p>extra',

        '<div align=justify><p>[foo]<p>bar</div><p>extra',
        '<div align=justify><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:justify><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:justify><p>[foo<p>bar]</div><p>extra',

        '<div align=left><p>[foo]<p>bar</div><p>extra',
        '<div align=left><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:left><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:left><p>[foo<p>bar]</div><p>extra',

        '<div align=right><p>[foo]<p>bar</div><p>extra',
        '<div align=right><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:right><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:right><p>[foo<p>bar]</div><p>extra',

        '<div align=left>foo</div>[bar]<p>extra',
        '[foo]<div align=left>bar</div><p>extra',
        '<div align=left>foo</div>[bar]<div align=left>baz</div><p>extra',
        '<div align=left><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div align=left><p>bar</div><p>extra',
        '<div align=left><p>foo</div><p>[bar]<div align=left><p>baz</div><p>extra',
        '<div style=text-align:left>foo</div>[bar]<p>extra',
        '[foo]<div style=text-align:left>bar</div><p>extra',
        '<div style=text-align:left>foo</div>[bar]<div style=text-align:left>baz</div><p>extra',
        '<div style=text-align:left><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div style=text-align:left><p>bar</div><p>extra',
        '<div style=text-align:left><p>foo</div><p>[bar]<div style=text-align:left><p>baz</div><p>extra',
        '<p align=left>foo<p>[bar]<p>extra',
        '<p>[foo]<p align=left>bar<p>extra',
        '<p align=left>foo<p>[bar]<p align=left>baz<p>extra',

        '<div align=left>[foo</div>bar]<p>extra',
        '<div align=left>fo[o</div>b]ar<p>extra',
        '<div style=text-align:left>[foo</div>bar]<p>extra',
        '<div style=text-align:left>fo[o</div>b]ar<p>extra',
        '<span style=text-align:left>[foo]</span><p>extra',
        '<span style=text-align:left>f[o]o</span><p>extra',

        '<div style=text-align:left>[foo<div style=text-align:left contenteditable=false>bar</div>baz]</div><p>extra',

        '<div align=nonsense><p>[foo]</div><p>extra',
        '<div style=text-align:inherit><p>[foo]</div><p>extra',
        '<quasit align=center><p>[foo]</p></quasit><p>extra',

        '<div style=text-align:start>[foo]</div><p>extra',
        '<div style=text-align:end>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:start>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:end>[foo]</div><p>extra',

        // Whitespace nodes
        '<div style=text-align:left><p>foo</div> <p>[bar]',
        '<div align=left><p>foo</div> <p>[bar]',
        '<p>[foo]</p> <div style=text-align:left><p>bar</div>',
        '<p>[foo]</p> <div align=left><p>bar</div>',
        '<div style=text-align:left><p>foo</div> <p>[bar]</p> <div style=text-align:left><p>baz</div>',
        '<div align=left><p>foo</div> <p>[bar]</p> <div align=left><p>baz</div>',
    ],
    //@}
    justifyright: [
    //@{
        'foo[]bar<p>extra',
        '<span>foo</span>{}<span>bar</span><p>extra',
        '<span>foo[</span><span>]bar</span><p>extra',
        'foo[bar]baz<p>extra',
        'foo[bar<b>baz]qoz</b>quz<p>extra',
        '<p>foo[]bar<p>extra',
        '<p>foo[bar]baz<p>extra',
        '<h1>foo[bar]baz</h1><p>extra',
        '<pre>foo[bar]baz</pre><p>extra',
        '<xmp>foo[bar]baz</xmp><p>extra',
        '<center><p>[foo]<p>bar</center><p>extra',
        '<center><p>[foo<p>bar]</center><p>extra',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table align=right><tbody><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table align=right><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=right><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=right><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table align=right data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table align=right><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody align=right><tr><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody align=right><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=right><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody align=right data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody align=right><tr><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tbody align=right><tr><td>foo<td>bar<td>baz</table>}<p>extra',

        '<table><tbody><tr align=right><td>foo<td>b[a]r<td>baz</table><p>extra',
        '<table><tbody><tr align=right data-start=1 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody><tr align=right data-start=0 data-end=2><td>foo<td>bar<td>baz</table><p>extra',
        '<table><tbody data-start=0 data-end=1><tr align=right><td>foo<td>bar<td>baz</table><p>extra',
        '<table data-start=0 data-end=1><tbody><tr align=right><td>foo<td>bar<td>baz</table><p>extra',
        '{<table><tr align=right><td>foo<td>bar<td>baz</table>}<p>extra',

        '<div align=center><p>[foo]<p>bar</div><p>extra',
        '<div align=center><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:center><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:center><p>[foo<p>bar]</div><p>extra',

        '<div align=justify><p>[foo]<p>bar</div><p>extra',
        '<div align=justify><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:justify><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:justify><p>[foo<p>bar]</div><p>extra',

        '<div align=left><p>[foo]<p>bar</div><p>extra',
        '<div align=left><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:left><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:left><p>[foo<p>bar]</div><p>extra',

        '<div align=right><p>[foo]<p>bar</div><p>extra',
        '<div align=right><p>[foo<p>bar}</div><p>extra',
        '<div style=text-align:right><p>[foo]<p>bar</div><p>extra',
        '<div style=text-align:right><p>[foo<p>bar]</div><p>extra',

        '<div align=right>foo</div>[bar]<p>extra',
        '[foo]<div align=right>bar</div><p>extra',
        '<div align=right>foo</div>[bar]<div align=right>baz</div><p>extra',
        '<div align=right><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div align=right><p>bar</div><p>extra',
        '<div align=right><p>foo</div><p>[bar]<div align=right><p>baz</div><p>extra',
        '<div style=text-align:right>foo</div>[bar]<p>extra',
        '[foo]<div style=text-align:right>bar</div><p>extra',
        '<div style=text-align:right>foo</div>[bar]<div style=text-align:right>baz</div><p>extra',
        '<div style=text-align:right><p>foo</div><p>[bar]<p>extra',
        '<p>[foo]<div style=text-align:right><p>bar</div><p>extra',
        '<div style=text-align:right><p>foo</div><p>[bar]<div style=text-align:right><p>baz</div><p>extra',
        '<p align=right>foo<p>[bar]<p>extra',
        '<p>[foo]<p align=right>bar<p>extra',
        '<p align=right>foo<p>[bar]<p align=right>baz<p>extra',

        '<div align=right>[foo</div>bar]<p>extra',
        '<div align=right>fo[o</div>b]ar<p>extra',
        '<div style=text-align:right>[foo</div>bar]<p>extra',
        '<div style=text-align:right>fo[o</div>b]ar<p>extra',
        '<span style=text-align:right>[foo]</span><p>extra',
        '<span style=text-align:right>f[o]o</span><p>extra',

        '<div style=text-align:right>[foo<div style=text-align:left contenteditable=false>bar</div>baz]</div><p>extra',

        '<div align=nonsense><p>[foo]</div><p>extra',
        '<div style=text-align:inherit><p>[foo]</div><p>extra',
        '<quasit align=center><p>[foo]</p></quasit><p>extra',

        '<div style=text-align:start>[foo]</div><p>extra',
        '<div style=text-align:end>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:start>[foo]</div><p>extra',
        '<div dir=rtl style=text-align:end>[foo]</div><p>extra',

        // Whitespace nodes
        '<div style=text-align:right><p>foo</div> <p>[bar]',
        '<div align=right><p>foo</div> <p>[bar]',
        '<p>[foo]</p> <div style=text-align:right><p>bar</div>',
        '<p>[foo]</p> <div align=right><p>bar</div>',
        '<div style=text-align:right><p>foo</div> <p>[bar]</p> <div style=text-align:right><p>baz</div>',
        '<div align=right><p>foo</div> <p>[bar]</p> <div align=right><p>baz</div>',
    ],
    //@}
    outdent: [
    //@{
        // These mimic existing indentation in various browsers, to see how
        // they cope with outdenting various things.  This is spec, Gecko
        // non-CSS, and Opera:
        '<blockquote><p>foo[bar]</p><p>baz</p></blockquote><p>extra',
        '<blockquote><p>foo[bar</p><p>b]az</p></blockquote><p>extra',
        '<blockquote><p>foo[bar]</p></blockquote><p>baz</p><p>extra',
        '<blockquote><p>foo[bar</p></blockquote><p>b]az</p><p>extra',

        // IE:
        '<blockquote style="margin-right: 0px;" dir="ltr"><p>foo[bar]</p><p>baz</p></blockquote><p>extra',
        '<blockquote style="margin-right: 0px;" dir="ltr"><p>foo[bar</p><p>b]az</p></blockquote><p>extra',
        '<blockquote style="margin-right: 0px;" dir="ltr"><p>foo[bar]</p></blockquote><p>baz</p><p>extra',
        '<blockquote style="margin-right: 0px;" dir="ltr"><p>foo[bar</p></blockquote><p>b]az</p><p>extra',

        // Firefox CSS mode:
        '<p style="margin-left: 40px">foo[bar]</p><p style="margin-left: 40px">baz</p><p>extra',
        '<p style="margin-left: 40px">foo[bar</p><p style="margin-left: 40px">b]az</p><p>extra',
        '<p style="margin-left: 40px">foo[bar]</p><p>baz</p><p>extra',
        '<p style="margin-left: 40px">foo[bar</p><p>b]az</p><p>extra',

        // WebKit:
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px;"><p>foo[bar]</p><p>baz</p></blockquote><p>extra',
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px;"><p>foo[bar</p><p>b]az</p></blockquote><p>extra',
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px;"><p>foo[bar]</p></blockquote><p>baz</p><p>extra',
        '<blockquote class="webkit-indent-blockquote" style="margin: 0 0 0 40px; border: none; padding: 0px;"><p>foo[bar</p></blockquote><p>b]az</p><p>extra',

        // Now let's try nesting lots of stuff and see what happens.
        '<blockquote><blockquote>foo[bar]baz</blockquote></blockquote>',
        '<blockquote><blockquote data-abc=def>foo[bar]baz</blockquote></blockquote>',
        '<blockquote data-abc=def><blockquote>foo[bar]baz</blockquote></blockquote>',
        '<blockquote><div>foo[bar]baz</div></blockquote>',
        '<blockquote><div id=abc>foo[bar]baz</div></blockquote>',
        '<blockquote id=abc>foo[bar]baz</blockquote>',
        '<blockquote style="color: blue">foo[bar]baz</blockquote>',

        '<blockquote><blockquote><p>foo[bar]<p>baz</blockquote></blockquote>',
        '<blockquote><blockquote data-abc=def><p>foo[bar]<p>baz</blockquote></blockquote>',
        '<blockquote data-abc=def><blockquote><p>foo[bar]<p>baz</blockquote></blockquote>',
        '<blockquote><div><p>foo[bar]<p>baz</div></blockquote>',
        '<blockquote><div id=abc><p>foo[bar]<p>baz</div></blockquote>',
        '<blockquote id=abc><p>foo[bar]<p>baz</blockquote>',
        '<blockquote style="color: blue"><p>foo[bar]<p>baz</blockquote>',

        '<blockquote><p><b>foo[bar]</b><p>baz</blockquote>',
        '<blockquote><p><strong>foo[bar]</strong><p>baz</blockquote>',
        '<blockquote><p><span>foo[bar]</span><p>baz</blockquote>',
        '<blockquote><blockquote style="color: blue"><p>foo[bar]</blockquote><p>baz</blockquote>',
        '<blockquote style="color: blue"><blockquote><p>foo[bar]</blockquote><p>baz</blockquote>',

        // Lists!
        '<ol><li>foo<li>[bar]<li>baz</ol>',
        '<ol data-start=1 data-end=2><li>foo<li>bar<li>baz</ol>',
        '<ol><li>foo</ol>[bar]',
        '<ol><li>[foo]<br>bar<li>baz</ol>',
        '<ol><li>foo<br>[bar]<li>baz</ol>',
        '<ol><li><div>[foo]</div>bar<li>baz</ol>',
        '<ol><li>foo<ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>[bar]<li>baz</ol><li>quz</ol>',
        '<ol><li>foo</li><ol data-start=0 data-end=1><li>bar<li>baz</ol><li>quz</ol>',
        '<ol><li>foo</li><ol><li>bar<li>[baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol data-start=1 data-end=2><li>bar<li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>b[a]r</ol><li>baz</ol>',
        '<ol><li>foo</li><ol><li>b[a]r</ol><li>baz</ol>',
        '<ol><li>foo{<ol><li>bar</ol>}<li>baz</ol>',
        '<ol><li>foo</li>{<ol><li>bar</ol>}<li>baz</ol>',
        '<ol><li>[foo]<ol><li>bar</ol><li>baz</ol>',
        '<ol><li>[foo]</li><ol><li>bar</ol><li>baz</ol>',
        '<ol><li>foo<li>[bar]<ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<li>[bar]</li><ol><li>baz</ol><li>quz</ol>',
        '<ol><li>foo<ol><li>bar<li>baz</ol><li>[quz]</ol>',
        '<ol><li>foo</li><ol><li>bar<li>baz</ol><li>[quz]</ol>',

        // Try outdenting multiple items at once.
        '<ol><li>foo<li>b[ar<li>baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol><li>baz</ol>',
        '<ol><li>[foo</li><ol><li>bar]</ol><li>baz</ol>',
        '<ol><li>foo<ol><li>b[ar</ol><li>b]az</ol>',
        '<ol><li>foo</li><ol><li>b[ar</ol><li>b]az</ol>',
        '<ol><li>[foo<ol><li>bar</ol><li>baz]</ol><p>extra',
        '<ol><li>[foo</li><ol><li>bar</ol><li>baz]</ol><p>extra',

        // We probably can't actually get this DOM . . .
        '<ol><li>[foo]<ol><li>bar</ol>baz</ol>',
        '<ol><li>foo<ol><li>[bar]</ol>baz</ol>',
        '<ol><li>foo<ol><li>bar</ol>[baz]</ol>',
        '<ol><li>[foo<ol><li>bar]</ol>baz</ol>',

        // Attribute handling on lists
        'foo<ol start=5><li>[bar]</ol>baz',
        'foo<ol id=abc><li>[bar]</ol>baz',
        'foo<ol style=color:blue><li>[bar]</ol>baz',
        'foo<ol><li value=5>[bar]</ol>baz',
        'foo<ol><li id=abc>[bar]</ol>baz',
        'foo<ol><li style=color:blue>[bar]</ol>baz',
        '<ol><li>foo</li><ol><li value=5>[bar]</ol></ol>',
        '<ul><li>foo</li><ol><li value=5>[bar]</ol></ul>',
        '<ol><li>foo</li><ol start=5><li>[bar]</ol><li>baz</ol>',
        '<ol><li>foo</li><ol id=abc><li>[bar]</ol><li>baz</ol>',
        '<ol><li>foo</li><ol style=color:blue><li>[bar]</ol><li>baz</ol>',
        '<ol><li>foo</li><ol style=text-indent:1em><li>[bar]</ol><li>baz</ol>',
        '<ol><li>foo</li><ol start=5><li>[bar<li>baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol id=abc><li>[bar<li>baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol style=color:blue><li>[bar<li>baz]</ol><li>quz</ol>',
        '<ol><li>foo</li><ol style=text-indent:1em><li>[bar<li>baz]</ol><li>quz</ol>',

        // List inside indentation element
        '<blockquote><ol><li>[foo]</ol></blockquote><p>extra',
        '<blockquote>foo<ol><li>[bar]</ol>baz</blockquote><p>extra',
        '<blockquote><ol><li>foo</li><ol><li>[bar]</ol><li>baz</ol></blockquote><p>extra',

        '<ol><li><h1>[foo]</h1></ol>',
        '<ol><li><xmp>[foo]</xmp></li></ol>',
        '<blockquote><ol><li>foo<div><ol><li>[bar]</ol></div><li>baz</ol></blockquote>',

        // Whitespace nodes
        '<blockquote> <p>[foo]</p></blockquote>',
        '<blockquote><p>[foo]</p> </blockquote>',
        '<blockquote> <p>[foo]</p> </blockquote>',
        '<ol> <li>[foo]</li></ol>',
        '<ol><li>[foo]</li> </ol>',
        '<ol> <li>[foo]</li> </ol>',
        '<ul> <li>[foo]</li></ul>',
        '<ul><li>[foo]</li> </ul>',
        '<ul> <li>[foo]</li> </ul>',
        '<blockquote> <p>[foo]</p> <p>bar</p> <p>baz</p></blockquote>',
        '<blockquote> <p>foo</p> <p>[bar]</p> <p>baz</p></blockquote>',
        '<blockquote> <p>foo</p> <p>bar</p> <p>[baz]</p></blockquote>',
        '<ol> <li>[foo]</li> <li>bar</li> <li>baz</li></ol>',
        '<ol> <li>foo</li> <li>[bar]</li> <li>baz</li></ol>',
        '<ol> <li>foo</li> <li>bar</li> <li>[baz]</li></ol>',
        '<ul> <li>[foo]</li> <li>bar</li> <li>baz</li></ul>',
        '<ul> <li>foo</li> <li>[bar]</li> <li>baz</li></ul>',
        '<ul> <li>foo</li> <li>bar</li> <li>[baz]</li></ul>',

        // https://bugs.webkit.org/show_bug.cgi?id=24249
        '<ol><li>[]a<table><tr><td><br></table></ol>',
        // https://bugs.webkit.org/show_bug.cgi?id=43447
        '<blockquote><span>foo<br>[bar]</span></blockquote>',
    ],
    //@}
    removeformat: [
    //@{
        'foo[]bar',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        '[foo<b>bar</b>baz]',
        'foo[<b>bar</b>baz]',
        'foo[<b>bar</b>]baz',
        'foo<b>[bar]</b>baz',
        'foo<b>b[a]r</b>baz',
        '[foo<strong>bar</strong>baz]',
        '[foo<span style="font-weight: bold">bar</span>baz]',
        'foo<span style="font-weight: bold">b[a]r</span>baz',
        '[foo<span style="font-variant: small-caps">bar</span>baz]',
        'foo<span style="font-variant: small-caps">b[a]r</span>baz',
        '[foo<b id=foo>bar</b>baz]',
        'foo<b id=foo>b[a]r</b>baz',

        // HTML has lots of inline elements, doesn't it?
        '[foo<a>bar</a>baz]',
        'foo<a>b[a]r</a>baz',
        '[foo<a href=foo>bar</a>baz]',
        'foo<a href=foo>b[a]r</a>baz',
        '[foo<abbr>bar</abbr>baz]',
        'foo<abbr>b[a]r</abbr>baz',
        '[foo<acronym>bar</acronym>baz]',
        'foo<acronym>b[a]r</acronym>baz',
        '[foo<b>bar</b>baz]',
        'foo<b>b[a]r</b>baz',
        '[foo<bdi dir=rtl>bar</bdi>baz]',
        'foo<bdi dir=rtl>b[a]r</bdi>baz',
        '[foo<bdo dir=rtl>bar</bdo>baz]',
        'foo<bdo dir=rtl>b[a]r</bdo>baz',
        '[foo<big>bar</big>baz]',
        'foo<big>b[a]r</big>baz',
        '[foo<blink>bar</blink>baz]',
        'foo<blink>b[a]r</blink>baz',
        '[foo<cite>bar</cite>baz]',
        'foo<cite>b[a]r</cite>baz',
        '[foo<code>bar</code>baz]',
        'foo<code>b[a]r</code>baz',
        '[foo<del>bar</del>baz]',
        'foo<del>b[a]r</del>baz',
        '[foo<dfn>bar</dfn>baz]',
        'foo<dfn>b[a]r</dfn>baz',
        '[foo<em>bar</em>baz]',
        'foo<em>b[a]r</em>baz',
        '[foo<font>bar</font>baz]',
        'foo<font>b[a]r</font>baz',
        '[foo<font color=blue>bar</font>baz]',
        'foo<font color=blue>b[a]r</font>baz',
        '[foo<i>bar</i>baz]',
        'foo<i>b[a]r</i>baz',
        '[foo<ins>bar</ins>baz]',
        'foo<ins>b[a]r</ins>baz',
        '[foo<kbd>bar</kbd>baz]',
        'foo<kbd>b[a]r</kbd>baz',
        '[foo<mark>bar</mark>baz]',
        'foo<mark>b[a]r</mark>baz',
        '[foo<nobr>bar</nobr>baz]',
        'foo<nobr>b[a]r</nobr>baz',
        '[foo<q>bar</q>baz]',
        'foo<q>b[a]r</q>baz',
        '[foo<samp>bar</samp>baz]',
        'foo<samp>b[a]r</samp>baz',
        '[foo<s>bar</s>baz]',
        'foo<s>b[a]r</s>baz',
        '[foo<small>bar</small>baz]',
        'foo<small>b[a]r</small>baz',
        '[foo<span>bar</span>baz]',
        'foo<span>b[a]r</span>baz',
        '[foo<strike>bar</strike>baz]',
        'foo<strike>b[a]r</strike>baz',
        '[foo<strong>bar</strong>baz]',
        'foo<strong>b[a]r</strong>baz',
        '[foo<sub>bar</sub>baz]',
        'foo<sub>b[a]r</sub>baz',
        '[foo<sup>bar</sup>baz]',
        'foo<sup>b[a]r</sup>baz',
        '[foo<tt>bar</tt>baz]',
        'foo<tt>b[a]r</tt>baz',
        '[foo<u>bar</u>baz]',
        'foo<u>b[a]r</u>baz',
        '[foo<var>bar</var>baz]',
        'foo<var>b[a]r</var>baz',

        // Empty and replaced elements
        '[foo<br>bar]',
        '[foo<hr>bar]',
        '[foo<wbr>bar]',
        '[foo<img>bar]',
        '[foo<img src=abc>bar]',
        '[foo<video></video>bar]',
        '[foo<video src=abc></video>bar]',
        '[foo<svg><circle fill=blue r=20 cx=20 cy=20 /></svg>bar]',

        // Unrecognized elements
        '[foo<nonexistentelement>bar</nonexistentelement>baz]',
        'foo<nonexistentelement>b[a]r</nonexistentelement>baz',
        '[foo<nonexistentelement style="display: block">bar</nonexistentelement>baz]',
        'foo<nonexistentelement style="display: block">b[a]r</nonexistentelement>baz',

        // Random stuff
        '[foo<span id=foo>bar</span>baz]',
        'foo<span id=foo>b[a]r</span>baz',
        '[foo<span class=foo>bar</span>baz]',
        'foo<span class=foo>b[a]r</span>baz',
        '[foo<b style="font-weight: normal">bar</b>baz]',
        'foo<b style="font-weight: normal">b[a]r</b>baz',
        '<p style="background-color: aqua">foo[bar]baz</p>',
        '<p><span style="background-color: aqua">foo[bar]baz</span></p>',
        '<p style="font-weight: bold">foo[bar]baz</p>',
        '<b><p style="font-weight: bold">foo[bar]baz</p></b>',
        '<p style="font-variant: small-caps">foo[bar]baz</p>',
        '{<p style="font-variant: small-caps">foobarbaz</p>}',
        '<p style="text-indent: 2em">foo[bar]baz</p>',
        '{<p style="text-indent: 2em">foobarbaz</p>}',

        // https://bugzilla.mozilla.org/show_bug.cgi?id=649138
        // Chrome 15 dev fails this for some unclear reason.
        '<table data-start=0 data-end=1><tr><td><b>foo</b></table>',
    ],
    //@}
    strikethrough: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<u>[bar]</u>baz',
        'foo<span style="text-decoration: underline">[bar]</span>baz',
        '<u>foo[bar]baz</u>',
        '<u>foo[b<span style="color:blue">ar]ba</span>z</u>',
        '<u>foo[b<span style="color:blue" id=foo>ar]ba</span>z</u>',
        '<u>foo[b<span style="font-size:3em">ar]ba</span>z</u>',
        '<u>foo[b<i>ar]ba</i>z</u>',
        '<p style="text-decoration: underline">foo[bar]baz</p>',

        'foo<s>[bar]</s>baz',
        'foo<span style="text-decoration: line-through">[bar]</span>baz',
        '<s>foo[bar]baz</s>',
        '<s>foo[b<span style="color:blue">ar]ba</span>z</s>',
        '<s>foo[b<span style="color:blue" id=foo>ar]ba</span>z</s>',
        '<s>foo[b<span style="font-size:3em">ar]ba</span>z</s>',
        '<s>foo[b<i>ar]ba</i>z</s>',
        '<p style="text-decoration: line-through">foo[bar]baz</p>',

        'foo<strike>[bar]</strike>baz',
        '<strike>foo[bar]baz</strike>',
        '<strike>foo[b<span style="color:blue">ar]ba</span>z</strike>',
        '<strike>foo[b<span style="color:blue" id=foo>ar]ba</span>z</strike>',
        '<strike>foo[b<span style="font-size:3em">ar]ba</span>z</strike>',
        '<strike>foo[b<i>ar]ba</i>z</strike>',

        'foo<ins>[bar]</ins>baz',
        '<ins>foo[bar]baz</ins>',
        '<ins>foo[b<span style="color:blue">ar]ba</span>z</ins>',
        '<ins>foo[b<span style="color:blue" id=foo>ar]ba</span>z</ins>',
        '<ins>foo[b<span style="font-size:3em">ar]ba</span>z</ins>',
        '<ins>foo[b<i>ar]ba</i>z</ins>',

        'foo<del>[bar]</del>baz',
        '<del>foo[bar]baz</del>',
        '<del>foo[b<span style="color:blue">ar]ba</span>z</del>',
        '<del>foo[b<span style="color:blue" id=foo>ar]ba</span>z</del>',
        '<del>foo[b<span style="font-size:3em">ar]ba</span>z</del>',
        '<del>foo[b<i>ar]ba</i>z</del>',

        'foo<span style="text-decoration: underline line-through">[bar]</span>baz',
        'foo<span style="text-decoration: underline line-through">b[a]r</span>baz',
        'foo<s style="text-decoration: underline">[bar]</s>baz',
        'foo<s style="text-decoration: underline">b[a]r</s>baz',
        'foo<u style="text-decoration: line-through">[bar]</u>baz',
        'foo<u style="text-decoration: line-through">b[a]r</u>baz',
        'foo<s style="text-decoration: overline">[bar]</s>baz',
        'foo<s style="text-decoration: overline">b[a]r</s>baz',
        'foo<u style="text-decoration: overline">[bar]</u>baz',
        'foo<u style="text-decoration: overline">b[a]r</u>baz',

        '<p style="text-decoration: line-through">foo[bar]baz</p>',
        '<p style="text-decoration: overline">foo[bar]baz</p>',

        'foo<span class="underline">[bar]</span>baz',
        'foo<span class="underline">b[a]r</span>baz',
        'foo<span class="line-through">[bar]</span>baz',
        'foo<span class="line-through">b[a]r</span>baz',
        'foo<span class="underline-and-line-through">[bar]</span>baz',
        'foo<span class="underline-and-line-through">b[a]r</span>baz',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<s>b]ar</s>baz',
        'foo<s>ba[r</s>b]az',
        'fo[o<s>bar</s>b]az',
        'foo[<s>b]ar</s>baz',
        'foo<s>ba[r</s>]baz',
        'foo[<s>bar</s>]baz',
        'foo<s>[bar]</s>baz',
        'foo{<s>bar</s>}baz',
        'fo[o<span style=text-decoration:line-through>b]ar</span>baz',
        '<strike>fo[o</strike><s>b]ar</s>',
        '<s>fo[o</s><del>b]ar</del>',
    ],
    //@}
    subscript: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<sub>[bar]</sub>baz',
        'foo<sub>b[a]r</sub>baz',
        'foo<sup>[bar]</sup>baz',
        'foo<sup>b[a]r</sup>baz',

        'foo<span style=vertical-align:sub>[bar]</span>baz',
        'foo<span style=vertical-align:super>[bar]</span>baz',

        'foo<sub><sub>[bar]</sub></sub>baz',
        'foo<sub><sub>b[a]r</sub></sub>baz',
        'foo<sub>b<sub>[a]</sub>r</sub>baz',
        'foo<sup><sup>[bar]</sup></sup>baz',
        'foo<sup><sup>b[a]r</sup></sup>baz',
        'foo<sup>b<sup>[a]</sup>r</sup>baz',
        'foo<sub><sup>[bar]</sup></sub>baz',
        'foo<sub><sup>b[a]r</sup></sub>baz',
        'foo<sub>b<sup>[a]</sup>r</sub>baz',
        'foo<sup><sub>[bar]</sub></sup>baz',
        'foo<sup><sub>b[a]r</sub></sup>baz',
        'foo<sup>b<sub>[a]</sub>r</sup>baz',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<sub>b]ar</sub>baz',
        'foo<sub>ba[r</sub>b]az',
        'fo[o<sub>bar</sub>b]az',
        'foo[<sub>b]ar</sub>baz',
        'foo<sub>ba[r</sub>]baz',
        'foo[<sub>bar</sub>]baz',
        'foo<sub>[bar]</sub>baz',
        'foo{<sub>bar</sub>}baz',
        '<sub>fo[o</sub><sup>b]ar</sup>',
        '<sub>fo[o</sub><span style=vertical-align:sub>b]ar</span>',
        'foo<span style=vertical-align:top>[bar]</span>baz',
        '<sub>fo[o</sub><span style=vertical-align:top>b]ar</span>',
    ],
    //@}
    superscript: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<sub>[bar]</sub>baz',
        'foo<sub>b[a]r</sub>baz',
        'foo<sup>[bar]</sup>baz',
        'foo<sup>b[a]r</sup>baz',

        'foo<span style=vertical-align:sub>[bar]</span>baz',
        'foo<span style=vertical-align:super>[bar]</span>baz',

        'foo<sub><sub>[bar]</sub></sub>baz',
        'foo<sub><sub>b[a]r</sub></sub>baz',
        'foo<sub>b<sub>[a]</sub>r</sub>baz',
        'foo<sup><sup>[bar]</sup></sup>baz',
        'foo<sup><sup>b[a]r</sup></sup>baz',
        'foo<sup>b<sup>[a]</sup>r</sup>baz',
        'foo<sub><sup>[bar]</sup></sub>baz',
        'foo<sub><sup>b[a]r</sup></sub>baz',
        'foo<sub>b<sup>[a]</sup>r</sub>baz',
        'foo<sup><sub>[bar]</sub></sup>baz',
        'foo<sup><sub>b[a]r</sub></sup>baz',
        'foo<sup>b<sub>[a]</sub>r</sup>baz',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<sup>b]ar</sup>baz',
        'foo<sup>ba[r</sup>b]az',
        'fo[o<sup>bar</sup>b]az',
        'foo[<sup>b]ar</sup>baz',
        'foo<sup>ba[r</sup>]baz',
        'foo[<sup>bar</sup>]baz',
        'foo<sup>[bar]</sup>baz',
        'foo{<sup>bar</sup>}baz',
        '<sup>fo[o</sup><sub>b]ar</sub>',
        '<sup>fo[o</sup><span style=vertical-align:super>b]ar</span>',
        'foo<span style=vertical-align:bottom>[bar]</span>baz',
        '<sup>fo[o</sup><span style=vertical-align:bottom>b]ar</span>',

        // https://bugs.webkit.org/show_bug.cgi?id=28472
        'foo<sup>[bar]<br></sup>',
    ],
    //@}
    underline: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<p>[foo<p><br><p>bar]',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<table><tbody><tr><td>foo<td>b[a]r<td>baz</table>',
        '<table><tbody><tr data-start=1 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody><tr data-start=0 data-end=2><td>foo<td>bar<td>baz</table>',
        '<table><tbody data-start=0 data-end=1><tr><td>foo<td>bar<td>baz</table>',
        '<table data-start=0 data-end=1><tbody><tr><td>foo<td>bar<td>baz</table>',
        '{<table><tr><td>foo<td>bar<td>baz</table>}',

        'foo<u>[bar]</u>baz',
        'foo<span style="text-decoration: underline">[bar]</span>baz',
        '<u>foo[bar]baz</u>',
        '<u>foo[b<span style="color:blue">ar]ba</span>z</u>',
        '<u>foo[b<span style="color:blue" id=foo>ar]ba</span>z</u>',
        '<u>foo[b<span style="font-size:3em">ar]ba</span>z</u>',
        '<u>foo[b<i>ar]ba</i>z</u>',
        '<p style="text-decoration: underline">foo[bar]baz</p>',

        'foo<s>[bar]</s>baz',
        'foo<span style="text-decoration: line-through">[bar]</span>baz',
        '<s>foo[bar]baz</s>',
        '<s>foo[b<span style="color:blue">ar]ba</span>z</s>',
        '<s>foo[b<span style="color:blue" id=foo>ar]ba</span>z</s>',
        '<s>foo[b<span style="font-size:3em">ar]ba</span>z</s>',
        '<s>foo[b<i>ar]ba</i>z</s>',
        '<p style="text-decoration: line-through">foo[bar]baz</p>',

        'foo<strike>[bar]</strike>baz',
        '<strike>foo[bar]baz</strike>',
        '<strike>foo[b<span style="color:blue">ar]ba</span>z</strike>',
        '<strike>foo[b<span style="color:blue" id=foo>ar]ba</span>z</strike>',
        '<strike>foo[b<span style="font-size:3em">ar]ba</span>z</strike>',
        '<strike>foo[b<i>ar]ba</i>z</strike>',

        'foo<ins>[bar]</ins>baz',
        '<ins>foo[bar]baz</ins>',
        '<ins>foo[b<span style="color:blue">ar]ba</span>z</ins>',
        '<ins>foo[b<span style="color:blue" id=foo>ar]ba</span>z</ins>',
        '<ins>foo[b<span style="font-size:3em">ar]ba</span>z</ins>',
        '<ins>foo[b<i>ar]ba</i>z</ins>',

        'foo<del>[bar]</del>baz',
        '<del>foo[bar]baz</del>',
        '<del>foo[b<span style="color:blue">ar]ba</span>z</del>',
        '<del>foo[b<span style="color:blue" id=foo>ar]ba</span>z</del>',
        '<del>foo[b<span style="font-size:3em">ar]ba</span>z</del>',
        '<del>foo[b<i>ar]ba</i>z</del>',

        'foo<span style="text-decoration: underline line-through">[bar]</span>baz',
        'foo<span style="text-decoration: underline line-through">b[a]r</span>baz',
        'foo<s style="text-decoration: underline">[bar]</s>baz',
        'foo<s style="text-decoration: underline">b[a]r</s>baz',
        'foo<u style="text-decoration: line-through">[bar]</u>baz',
        'foo<u style="text-decoration: line-through">b[a]r</u>baz',
        'foo<s style="text-decoration: overline">[bar]</s>baz',
        'foo<s style="text-decoration: overline">b[a]r</s>baz',
        'foo<u style="text-decoration: overline">[bar]</u>baz',
        'foo<u style="text-decoration: overline">b[a]r</u>baz',

        '<p style="text-decoration: line-through">foo[bar]baz</p>',
        '<p style="text-decoration: overline">foo[bar]baz</p>',

        'foo<span class="underline">[bar]</span>baz',
        'foo<span class="underline">b[a]r</span>baz',
        'foo<span class="line-through">[bar]</span>baz',
        'foo<span class="line-through">b[a]r</span>baz',
        'foo<span class="underline-and-line-through">[bar]</span>baz',
        'foo<span class="underline-and-line-through">b[a]r</span>baz',

        // Tests for queryCommandIndeterm() and queryCommandState()
        'fo[o<u>b]ar</u>baz',
        'foo<u>ba[r</u>b]az',
        'fo[o<u>bar</u>b]az',
        'foo[<u>b]ar</u>baz',
        'foo<u>ba[r</u>]baz',
        'foo[<u>bar</u>]baz',
        'foo<u>[bar]</u>baz',
        'foo{<u>bar</u>}baz',
        'fo[o<span style=text-decoration:underline>b]ar</span>baz',
        '<ins>fo[o</ins><u>b]ar</u>',
        '<u>fo[o</u><ins>b]ar</ins>',
    ],
    //@}
    unlink: [
    //@{
        'foo[]bar',
        '<p>[foo</p> <p>bar]</p>',
        '<span>[foo</span> <span>bar]</span>',
        '<p>[foo</p><p> <span>bar</span> </p><p>baz]</p>',
        '<b>foo[]bar</b>',
        '<i>foo[]bar</i>',
        '<span>foo</span>{}<span>bar</span>',
        '<span>foo[</span><span>]bar</span>',
        'foo[bar]baz',
        'foo[bar<b>baz]qoz</b>quz',
        'foo[bar<i>baz]qoz</i>quz',
        '{<p><p> <p>foo</p>}',

        '<a href=http://www.google.com/>foo[bar]baz</a>',
        '<a href=http://www.google.com/>foo[barbaz</a>}',
        '{<a href=http://www.google.com/>foobar]baz</a>',
        '{<a href=http://www.google.com/>foobarbaz</a>}',
        '<a href=http://www.google.com/>[foobarbaz]</a>',

        'foo<a href=http://www.google.com/>b[]ar</a>baz',
        'foo<a href=http://www.google.com/>[bar]</a>baz',
        'foo[<a href=http://www.google.com/>bar</a>]baz',
        'foo<a href=http://www.google.com/>[bar</a>baz]',
        '[foo<a href=http://www.google.com/>bar]</a>baz',
        '[foo<a href=http://www.google.com/>bar</a>baz]',

        '<a id=foo href=http://www.google.com/>foobar[]baz</a>',
        '<a id=foo href=http://www.google.com/>foo[bar]baz</a>',
        '<a id=foo href=http://www.google.com/>[foobarbaz]</a>',
        'foo<a id=foo href=http://www.google.com/>[bar]</a>baz',
        'foo[<a id=foo href=http://www.google.com/>bar</a>]baz',
        '[foo<a id=foo href=http://www.google.com/>bar</a>baz]',

        '<a name=foo>foobar[]baz</a>',
        '<a name=foo>foo[bar]baz</a>',
        '<a name=foo>[foobarbaz]</a>',
        'foo<a name=foo>[bar]</a>baz',
        'foo[<a name=foo>bar</a>]baz',
        '[foo<a name=foo>bar</a>baz]',
    ],
    //@}
    copy: ['!foo[bar]baz'],
    cut: ['!foo[bar]baz'],
    defaultparagraphseparator: [
    //@{
        ['', 'foo[bar]baz'],
        ['div', 'foo[bar]baz'],
        ['p', 'foo[bar]baz'],
        ['DIV', 'foo[bar]baz'],
        ['P', 'foo[bar]baz'],
        [' div ', 'foo[bar]baz'],
        [' p ', 'foo[bar]baz'],
        ['<div>', 'foo[bar]baz'],
        ['<p>', 'foo[bar]baz'],
        ['li', 'foo[bar]baz'],
        ['blockquote', 'foo[bar]baz'],
    ],
    //@}
    paste: ['!foo[bar]baz'],
    selectall: ['foo[bar]baz'],
    stylewithcss: [
    //@{
        ['true', 'foo[bar]baz'],
        ['TRUE', 'foo[bar]baz'],
        ['TrUe', 'foo[bar]baz'],
        ['true ', 'foo[bar]baz'],
        [' true', 'foo[bar]baz'],
        ['truer', 'foo[bar]baz'],
        [' true ', 'foo[bar]baz'],
        [' TrUe', 'foo[bar]baz'],
        ['', 'foo[bar]baz'],
        [' ', 'foo[bar]baz'],
        ['false', 'foo[bar]baz'],
        ['FALSE', 'foo[bar]baz'],
        ['FaLsE', 'foo[bar]baz'],
        [' false', 'foo[bar]baz'],
        ['false ', 'foo[bar]baz'],
        ['falser', 'foo[bar]baz'],
        ['falsé', 'foo[bar]baz'],
    ],
    //@}
    usecss: [
    //@{
        ['true', 'foo[bar]baz'],
        ['TRUE', 'foo[bar]baz'],
        ['TrUe', 'foo[bar]baz'],
        ['true ', 'foo[bar]baz'],
        [' true', 'foo[bar]baz'],
        ['truer', 'foo[bar]baz'],
        [' true ', 'foo[bar]baz'],
        [' TrUe', 'foo[bar]baz'],
        ['', 'foo[bar]baz'],
        [' ', 'foo[bar]baz'],
        ['false', 'foo[bar]baz'],
        ['FALSE', 'foo[bar]baz'],
        ['FaLsE', 'foo[bar]baz'],
        [' false', 'foo[bar]baz'],
        ['false ', 'foo[bar]baz'],
        ['falser', 'foo[bar]baz'],
        ['falsé', 'foo[bar]baz'],
    ],
    //@}
    quasit: ['foo[bar]baz'],
    multitest: [
    //@{
        // Insertion-affecting state.  Test that insertText works right, and
        // test that various block commands preserve (or don't preserve) the
        // state.
        ['foo[]bar', 'bold', 'inserttext'],
        ['foo[]bar', 'bold', 'delete'],
        ['foo[]bar', 'bold', 'delete', 'inserttext'],
        ['foo[]bar', 'bold', 'formatblock'],
        ['foo[]bar', 'bold', 'formatblock', 'inserttext'],
        ['foo[]bar', 'bold', 'forwarddelete'],
        ['foo[]bar', 'bold', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'bold', 'indent'],
        ['foo[]bar', 'bold', 'indent', 'inserttext'],
        ['foo[]bar', 'bold', 'inserthorizontalrule'],
        ['foo[]bar', 'bold', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'bold', 'inserthtml'],
        ['foo[]bar', 'bold', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'bold', 'insertimage'],
        ['foo[]bar', 'bold', 'insertimage', 'inserttext'],
        ['foo[]bar', 'bold', 'insertlinebreak'],
        ['foo[]bar', 'bold', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'bold', 'insertorderedlist'],
        ['foo[]bar', 'bold', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'bold', 'insertparagraph'],
        ['foo[]bar', 'bold', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'bold', 'insertunorderedlist'],
        ['foo[]bar', 'bold', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'bold', 'justifycenter'],
        ['foo[]bar', 'bold', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'bold', 'justifyfull'],
        ['foo[]bar', 'bold', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'bold', 'justifyleft'],
        ['foo[]bar', 'bold', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'bold', 'justifyright'],
        ['foo[]bar', 'bold', 'justifyright', 'inserttext'],
        ['foo[]bar', 'bold', 'outdent'],
        ['foo[]bar', 'bold', 'outdent', 'inserttext'],

        ['foo[]bar', 'italic', 'inserttext'],
        ['foo[]bar', 'italic', 'delete'],
        ['foo[]bar', 'italic', 'delete', 'inserttext'],
        ['foo[]bar', 'italic', 'formatblock'],
        ['foo[]bar', 'italic', 'formatblock', 'inserttext'],
        ['foo[]bar', 'italic', 'forwarddelete'],
        ['foo[]bar', 'italic', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'italic', 'indent'],
        ['foo[]bar', 'italic', 'indent', 'inserttext'],
        ['foo[]bar', 'italic', 'inserthorizontalrule'],
        ['foo[]bar', 'italic', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'italic', 'inserthtml'],
        ['foo[]bar', 'italic', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'italic', 'insertimage'],
        ['foo[]bar', 'italic', 'insertimage', 'inserttext'],
        ['foo[]bar', 'italic', 'insertlinebreak'],
        ['foo[]bar', 'italic', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'italic', 'insertorderedlist'],
        ['foo[]bar', 'italic', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'italic', 'insertparagraph'],
        ['foo[]bar', 'italic', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'italic', 'insertunorderedlist'],
        ['foo[]bar', 'italic', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'italic', 'justifycenter'],
        ['foo[]bar', 'italic', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'italic', 'justifyfull'],
        ['foo[]bar', 'italic', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'italic', 'justifyleft'],
        ['foo[]bar', 'italic', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'italic', 'justifyright'],
        ['foo[]bar', 'italic', 'justifyright', 'inserttext'],
        ['foo[]bar', 'italic', 'outdent'],
        ['foo[]bar', 'italic', 'outdent', 'inserttext'],

        ['foo[]bar', 'strikethrough', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'delete'],
        ['foo[]bar', 'strikethrough', 'delete', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'formatblock'],
        ['foo[]bar', 'strikethrough', 'formatblock', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'forwarddelete'],
        ['foo[]bar', 'strikethrough', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'indent'],
        ['foo[]bar', 'strikethrough', 'indent', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'inserthorizontalrule'],
        ['foo[]bar', 'strikethrough', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'inserthtml'],
        ['foo[]bar', 'strikethrough', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'insertimage'],
        ['foo[]bar', 'strikethrough', 'insertimage', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'insertlinebreak'],
        ['foo[]bar', 'strikethrough', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'insertorderedlist'],
        ['foo[]bar', 'strikethrough', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'insertparagraph'],
        ['foo[]bar', 'strikethrough', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'insertunorderedlist'],
        ['foo[]bar', 'strikethrough', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'justifycenter'],
        ['foo[]bar', 'strikethrough', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'justifyfull'],
        ['foo[]bar', 'strikethrough', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'justifyleft'],
        ['foo[]bar', 'strikethrough', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'justifyright'],
        ['foo[]bar', 'strikethrough', 'justifyright', 'inserttext'],
        ['foo[]bar', 'strikethrough', 'outdent'],
        ['foo[]bar', 'strikethrough', 'outdent', 'inserttext'],

        ['foo[]bar', 'subscript', 'inserttext'],
        ['foo[]bar', 'subscript', 'delete'],
        ['foo[]bar', 'subscript', 'delete', 'inserttext'],
        ['foo[]bar', 'subscript', 'formatblock'],
        ['foo[]bar', 'subscript', 'formatblock', 'inserttext'],
        ['foo[]bar', 'subscript', 'forwarddelete'],
        ['foo[]bar', 'subscript', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'subscript', 'indent'],
        ['foo[]bar', 'subscript', 'indent', 'inserttext'],
        ['foo[]bar', 'subscript', 'inserthorizontalrule'],
        ['foo[]bar', 'subscript', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'subscript', 'inserthtml'],
        ['foo[]bar', 'subscript', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'subscript', 'insertimage'],
        ['foo[]bar', 'subscript', 'insertimage', 'inserttext'],
        ['foo[]bar', 'subscript', 'insertlinebreak'],
        ['foo[]bar', 'subscript', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'subscript', 'insertorderedlist'],
        ['foo[]bar', 'subscript', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'subscript', 'insertparagraph'],
        ['foo[]bar', 'subscript', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'subscript', 'insertunorderedlist'],
        ['foo[]bar', 'subscript', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'subscript', 'justifycenter'],
        ['foo[]bar', 'subscript', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'subscript', 'justifyfull'],
        ['foo[]bar', 'subscript', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'subscript', 'justifyleft'],
        ['foo[]bar', 'subscript', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'subscript', 'justifyright'],
        ['foo[]bar', 'subscript', 'justifyright', 'inserttext'],
        ['foo[]bar', 'subscript', 'outdent'],
        ['foo[]bar', 'subscript', 'outdent', 'inserttext'],

        ['foo[]bar', 'superscript', 'inserttext'],
        ['foo[]bar', 'superscript', 'delete'],
        ['foo[]bar', 'superscript', 'delete', 'inserttext'],
        ['foo[]bar', 'superscript', 'formatblock'],
        ['foo[]bar', 'superscript', 'formatblock', 'inserttext'],
        ['foo[]bar', 'superscript', 'forwarddelete'],
        ['foo[]bar', 'superscript', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'superscript', 'indent'],
        ['foo[]bar', 'superscript', 'indent', 'inserttext'],
        ['foo[]bar', 'superscript', 'inserthorizontalrule'],
        ['foo[]bar', 'superscript', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'superscript', 'inserthtml'],
        ['foo[]bar', 'superscript', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'superscript', 'insertimage'],
        ['foo[]bar', 'superscript', 'insertimage', 'inserttext'],
        ['foo[]bar', 'superscript', 'insertlinebreak'],
        ['foo[]bar', 'superscript', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'superscript', 'insertorderedlist'],
        ['foo[]bar', 'superscript', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'superscript', 'insertparagraph'],
        ['foo[]bar', 'superscript', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'superscript', 'insertunorderedlist'],
        ['foo[]bar', 'superscript', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'superscript', 'justifycenter'],
        ['foo[]bar', 'superscript', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'superscript', 'justifyfull'],
        ['foo[]bar', 'superscript', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'superscript', 'justifyleft'],
        ['foo[]bar', 'superscript', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'superscript', 'justifyright'],
        ['foo[]bar', 'superscript', 'justifyright', 'inserttext'],
        ['foo[]bar', 'superscript', 'outdent'],
        ['foo[]bar', 'superscript', 'outdent', 'inserttext'],

        ['foo[]bar', 'underline', 'inserttext'],
        ['foo[]bar', 'underline', 'delete'],
        ['foo[]bar', 'underline', 'delete', 'inserttext'],
        ['foo[]bar', 'underline', 'formatblock'],
        ['foo[]bar', 'underline', 'formatblock', 'inserttext'],
        ['foo[]bar', 'underline', 'forwarddelete'],
        ['foo[]bar', 'underline', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'underline', 'indent'],
        ['foo[]bar', 'underline', 'indent', 'inserttext'],
        ['foo[]bar', 'underline', 'inserthorizontalrule'],
        ['foo[]bar', 'underline', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'underline', 'inserthtml'],
        ['foo[]bar', 'underline', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'underline', 'insertimage'],
        ['foo[]bar', 'underline', 'insertimage', 'inserttext'],
        ['foo[]bar', 'underline', 'insertlinebreak'],
        ['foo[]bar', 'underline', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'underline', 'insertorderedlist'],
        ['foo[]bar', 'underline', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'underline', 'insertparagraph'],
        ['foo[]bar', 'underline', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'underline', 'insertunorderedlist'],
        ['foo[]bar', 'underline', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'underline', 'justifycenter'],
        ['foo[]bar', 'underline', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'underline', 'justifyfull'],
        ['foo[]bar', 'underline', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'underline', 'justifyleft'],
        ['foo[]bar', 'underline', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'underline', 'justifyright'],
        ['foo[]bar', 'underline', 'justifyright', 'inserttext'],
        ['foo[]bar', 'underline', 'outdent'],
        ['foo[]bar', 'underline', 'outdent', 'inserttext'],

        // Insertion-affecting value.  Test that insertText works right, and
        // test that various block commands preserve (or don't preserve) the
        // value.
        ['foo[]bar', 'backcolor', 'inserttext'],
        ['foo[]bar', 'backcolor', 'delete'],
        ['foo[]bar', 'backcolor', 'delete', 'inserttext'],
        ['foo[]bar', 'backcolor', 'formatblock'],
        ['foo[]bar', 'backcolor', 'formatblock', 'inserttext'],
        ['foo[]bar', 'backcolor', 'forwarddelete'],
        ['foo[]bar', 'backcolor', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'backcolor', 'indent'],
        ['foo[]bar', 'backcolor', 'indent', 'inserttext'],
        ['foo[]bar', 'backcolor', 'inserthorizontalrule'],
        ['foo[]bar', 'backcolor', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'backcolor', 'inserthtml'],
        ['foo[]bar', 'backcolor', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'backcolor', 'insertimage'],
        ['foo[]bar', 'backcolor', 'insertimage', 'inserttext'],
        ['foo[]bar', 'backcolor', 'insertlinebreak'],
        ['foo[]bar', 'backcolor', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'backcolor', 'insertorderedlist'],
        ['foo[]bar', 'backcolor', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'backcolor', 'insertparagraph'],
        ['foo[]bar', 'backcolor', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'backcolor', 'insertunorderedlist'],
        ['foo[]bar', 'backcolor', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'backcolor', 'justifycenter'],
        ['foo[]bar', 'backcolor', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'backcolor', 'justifyfull'],
        ['foo[]bar', 'backcolor', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'backcolor', 'justifyleft'],
        ['foo[]bar', 'backcolor', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'backcolor', 'justifyright'],
        ['foo[]bar', 'backcolor', 'justifyright', 'inserttext'],
        ['foo[]bar', 'backcolor', 'outdent'],
        ['foo[]bar', 'backcolor', 'outdent', 'inserttext'],

        ['foo[]bar', 'createlink', 'inserttext'],
        ['foo[]bar', 'createlink', 'delete'],
        ['foo[]bar', 'createlink', 'delete', 'inserttext'],
        ['foo[]bar', 'createlink', 'formatblock'],
        ['foo[]bar', 'createlink', 'formatblock', 'inserttext'],
        ['foo[]bar', 'createlink', 'forwarddelete'],
        ['foo[]bar', 'createlink', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'createlink', 'indent'],
        ['foo[]bar', 'createlink', 'indent', 'inserttext'],
        ['foo[]bar', 'createlink', 'inserthorizontalrule'],
        ['foo[]bar', 'createlink', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'createlink', 'inserthtml'],
        ['foo[]bar', 'createlink', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'createlink', 'insertimage'],
        ['foo[]bar', 'createlink', 'insertimage', 'inserttext'],
        ['foo[]bar', 'createlink', 'insertlinebreak'],
        ['foo[]bar', 'createlink', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'createlink', 'insertorderedlist'],
        ['foo[]bar', 'createlink', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'createlink', 'insertparagraph'],
        ['foo[]bar', 'createlink', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'createlink', 'insertunorderedlist'],
        ['foo[]bar', 'createlink', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'createlink', 'justifycenter'],
        ['foo[]bar', 'createlink', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'createlink', 'justifyfull'],
        ['foo[]bar', 'createlink', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'createlink', 'justifyleft'],
        ['foo[]bar', 'createlink', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'createlink', 'justifyright'],
        ['foo[]bar', 'createlink', 'justifyright', 'inserttext'],
        ['foo[]bar', 'createlink', 'outdent'],
        ['foo[]bar', 'createlink', 'outdent', 'inserttext'],

        ['foo[]bar', 'fontname', 'inserttext'],
        ['foo[]bar', 'fontname', 'delete'],
        ['foo[]bar', 'fontname', 'delete', 'inserttext'],
        ['foo[]bar', 'fontname', 'formatblock'],
        ['foo[]bar', 'fontname', 'formatblock', 'inserttext'],
        ['foo[]bar', 'fontname', 'forwarddelete'],
        ['foo[]bar', 'fontname', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'fontname', 'indent'],
        ['foo[]bar', 'fontname', 'indent', 'inserttext'],
        ['foo[]bar', 'fontname', 'inserthorizontalrule'],
        ['foo[]bar', 'fontname', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'fontname', 'inserthtml'],
        ['foo[]bar', 'fontname', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'fontname', 'insertimage'],
        ['foo[]bar', 'fontname', 'insertimage', 'inserttext'],
        ['foo[]bar', 'fontname', 'insertlinebreak'],
        ['foo[]bar', 'fontname', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'fontname', 'insertorderedlist'],
        ['foo[]bar', 'fontname', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'fontname', 'insertparagraph'],
        ['foo[]bar', 'fontname', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'fontname', 'insertunorderedlist'],
        ['foo[]bar', 'fontname', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'fontname', 'justifycenter'],
        ['foo[]bar', 'fontname', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'fontname', 'justifyfull'],
        ['foo[]bar', 'fontname', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'fontname', 'justifyleft'],
        ['foo[]bar', 'fontname', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'fontname', 'justifyright'],
        ['foo[]bar', 'fontname', 'justifyright', 'inserttext'],
        ['foo[]bar', 'fontname', 'outdent'],
        ['foo[]bar', 'fontname', 'outdent', 'inserttext'],

        ['foo[]bar', 'fontsize', 'inserttext'],
        ['foo[]bar', 'fontsize', 'delete'],
        ['foo[]bar', 'fontsize', 'delete', 'inserttext'],
        ['foo[]bar', 'fontsize', 'formatblock'],
        ['foo[]bar', 'fontsize', 'formatblock', 'inserttext'],
        ['foo[]bar', 'fontsize', 'forwarddelete'],
        ['foo[]bar', 'fontsize', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'fontsize', 'indent'],
        ['foo[]bar', 'fontsize', 'indent', 'inserttext'],
        ['foo[]bar', 'fontsize', 'inserthorizontalrule'],
        ['foo[]bar', 'fontsize', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'fontsize', 'inserthtml'],
        ['foo[]bar', 'fontsize', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'fontsize', 'insertimage'],
        ['foo[]bar', 'fontsize', 'insertimage', 'inserttext'],
        ['foo[]bar', 'fontsize', 'insertlinebreak'],
        ['foo[]bar', 'fontsize', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'fontsize', 'insertorderedlist'],
        ['foo[]bar', 'fontsize', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'fontsize', 'insertparagraph'],
        ['foo[]bar', 'fontsize', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'fontsize', 'insertunorderedlist'],
        ['foo[]bar', 'fontsize', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'fontsize', 'justifycenter'],
        ['foo[]bar', 'fontsize', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'fontsize', 'justifyfull'],
        ['foo[]bar', 'fontsize', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'fontsize', 'justifyleft'],
        ['foo[]bar', 'fontsize', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'fontsize', 'justifyright'],
        ['foo[]bar', 'fontsize', 'justifyright', 'inserttext'],
        ['foo[]bar', 'fontsize', 'outdent'],
        ['foo[]bar', 'fontsize', 'outdent', 'inserttext'],

        ['foo[]bar', 'forecolor', 'inserttext'],
        ['foo[]bar', 'forecolor', 'delete'],
        ['foo[]bar', 'forecolor', 'delete', 'inserttext'],
        ['foo[]bar', 'forecolor', 'formatblock'],
        ['foo[]bar', 'forecolor', 'formatblock', 'inserttext'],
        ['foo[]bar', 'forecolor', 'forwarddelete'],
        ['foo[]bar', 'forecolor', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'forecolor', 'indent'],
        ['foo[]bar', 'forecolor', 'indent', 'inserttext'],
        ['foo[]bar', 'forecolor', 'inserthorizontalrule'],
        ['foo[]bar', 'forecolor', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'forecolor', 'inserthtml'],
        ['foo[]bar', 'forecolor', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'forecolor', 'insertimage'],
        ['foo[]bar', 'forecolor', 'insertimage', 'inserttext'],
        ['foo[]bar', 'forecolor', 'insertlinebreak'],
        ['foo[]bar', 'forecolor', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'forecolor', 'insertorderedlist'],
        ['foo[]bar', 'forecolor', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'forecolor', 'insertparagraph'],
        ['foo[]bar', 'forecolor', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'forecolor', 'insertunorderedlist'],
        ['foo[]bar', 'forecolor', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'forecolor', 'justifycenter'],
        ['foo[]bar', 'forecolor', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'forecolor', 'justifyfull'],
        ['foo[]bar', 'forecolor', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'forecolor', 'justifyleft'],
        ['foo[]bar', 'forecolor', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'forecolor', 'justifyright'],
        ['foo[]bar', 'forecolor', 'justifyright', 'inserttext'],
        ['foo[]bar', 'forecolor', 'outdent'],
        ['foo[]bar', 'forecolor', 'outdent', 'inserttext'],

        ['foo[]bar', 'hilitecolor', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'delete'],
        ['foo[]bar', 'hilitecolor', 'delete', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'formatblock'],
        ['foo[]bar', 'hilitecolor', 'formatblock', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'forwarddelete'],
        ['foo[]bar', 'hilitecolor', 'forwarddelete', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'indent'],
        ['foo[]bar', 'hilitecolor', 'indent', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'inserthorizontalrule'],
        ['foo[]bar', 'hilitecolor', 'inserthorizontalrule', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'inserthtml'],
        ['foo[]bar', 'hilitecolor', 'inserthtml', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'insertimage'],
        ['foo[]bar', 'hilitecolor', 'insertimage', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'insertlinebreak'],
        ['foo[]bar', 'hilitecolor', 'insertlinebreak', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'insertorderedlist'],
        ['foo[]bar', 'hilitecolor', 'insertorderedlist', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'insertparagraph'],
        ['foo[]bar', 'hilitecolor', 'insertparagraph', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'insertunorderedlist'],
        ['foo[]bar', 'hilitecolor', 'insertunorderedlist', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'justifycenter'],
        ['foo[]bar', 'hilitecolor', 'justifycenter', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'justifyfull'],
        ['foo[]bar', 'hilitecolor', 'justifyfull', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'justifyleft'],
        ['foo[]bar', 'hilitecolor', 'justifyleft', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'justifyright'],
        ['foo[]bar', 'hilitecolor', 'justifyright', 'inserttext'],
        ['foo[]bar', 'hilitecolor', 'outdent'],
        ['foo[]bar', 'hilitecolor', 'outdent', 'inserttext'],

        // Test things that interfere with each other
        ['foo[]bar', 'superscript', 'subscript', 'inserttext'],
        ['foo[]bar', 'subscript', 'superscript', 'inserttext'],

        ['foo[]bar', 'createlink', ['forecolor', '#0000FF'], 'inserttext'],
        ['foo[]bar', ['forecolor', '#0000FF'], 'createlink', 'inserttext'],
        ['foo[]bar', 'createlink', ['forecolor', 'blue'], 'inserttext'],
        ['foo[]bar', ['forecolor', 'blue'], 'createlink', 'inserttext'],
        ['foo[]bar', 'createlink', ['forecolor', 'brown'], 'inserttext'],
        ['foo[]bar', ['forecolor', 'brown'], 'createlink', 'inserttext'],
        ['foo[]bar', 'createlink', ['forecolor', 'black'], 'inserttext'],
        ['foo[]bar', ['forecolor', 'black'], 'createlink', 'inserttext'],
        ['foo[]bar', 'createlink', 'underline', 'inserttext'],
        ['foo[]bar', 'underline', 'createlink', 'inserttext'],
        ['foo[]bar', 'createlink', 'underline', 'underline', 'inserttext'],
        ['foo[]bar', 'underline', 'underline', 'createlink', 'inserttext'],

        ['foo[]bar', 'subscript', ['fontsize', '2'], 'inserttext'],
        ['foo[]bar', ['fontsize', '2'], 'subscript', 'inserttext'],
        ['foo[]bar', 'subscript', ['fontsize', '3'], 'inserttext'],
        ['foo[]bar', ['fontsize', '3'], 'subscript', 'inserttext'],

        ['foo[]bar', ['hilitecolor', 'aqua'], ['backcolor', 'tan'], 'inserttext'],
        ['foo[]bar', ['backcolor', 'tan'], ['hilitecolor', 'aqua'], 'inserttext'],


        // The following are all just inserttext tests that we took from there,
        // but we first backspace the selected text instead of letting
        // inserttext handle it.  This tests that deletion correctly sets
        // overrides.
        ['foo<b>[bar]</b>baz', 'delete', 'inserttext'],
        ['foo<i>[bar]</i>baz', 'delete', 'inserttext'],
        ['foo<s>[bar]</s>baz', 'delete', 'inserttext'],
        ['foo<sub>[bar]</sub>baz', 'delete', 'inserttext'],
        ['foo<sup>[bar]</sup>baz', 'delete', 'inserttext'],
        ['foo<u>[bar]</u>baz', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com>[bar]</a>baz', 'delete', 'inserttext'],
        ['foo<font face=sans-serif>[bar]</font>baz', 'delete', 'inserttext'],
        ['foo<font size=4>[bar]</font>baz', 'delete', 'inserttext'],
        ['foo<font color=#0000FF>[bar]</font>baz', 'delete', 'inserttext'],
        ['foo<span style=background-color:#00FFFF>[bar]</span>baz', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><font color=blue>[bar]</font></a>baz', 'delete', 'inserttext'],
        ['foo<font color=blue><a href=http://www.google.com>[bar]</a></font>baz', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><font color=brown>[bar]</font></a>baz', 'delete', 'inserttext'],
        ['foo<font color=brown><a href=http://www.google.com>[bar]</a></font>baz', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><font color=black>[bar]</font></a>baz', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><u>[bar]</u></a>baz', 'delete', 'inserttext'],
        ['foo<u><a href=http://www.google.com>[bar]</a></u>baz', 'delete', 'inserttext'],
        ['foo<sub><font size=2>[bar]</font></sub>baz', 'delete', 'inserttext'],
        ['foo<font size=2><sub>[bar]</sub></font>baz', 'delete', 'inserttext'],
        ['foo<sub><font size=3>[bar]</font></sub>baz', 'delete', 'inserttext'],
        ['foo<font size=3><sub>[bar]</sub></font>baz', 'delete', 'inserttext'],

        // Now repeat but with different selections.
        ['[foo<b>bar]</b>baz', 'delete', 'inserttext'],
        ['[foo<i>bar]</i>baz', 'delete', 'inserttext'],
        ['[foo<s>bar]</s>baz', 'delete', 'inserttext'],
        ['[foo<sub>bar]</sub>baz', 'delete', 'inserttext'],
        ['[foo<sup>bar]</sup>baz', 'delete', 'inserttext'],
        ['[foo<u>bar]</u>baz', 'delete', 'inserttext'],
        ['[foo<a href=http://www.google.com>bar]</a>baz', 'delete', 'inserttext'],
        ['[foo<font face=sans-serif>bar]</font>baz', 'delete', 'inserttext'],
        ['[foo<font size=4>bar]</font>baz', 'delete', 'inserttext'],
        ['[foo<font color=#0000FF>bar]</font>baz', 'delete', 'inserttext'],
        ['[foo<span style=background-color:#00FFFF>bar]</span>baz', 'delete', 'inserttext'],
        ['[foo<a href=http://www.google.com><font color=blue>bar]</font></a>baz', 'delete', 'inserttext'],
        ['[foo<font color=blue><a href=http://www.google.com>bar]</a></font>baz', 'delete', 'inserttext'],
        ['[foo<a href=http://www.google.com><font color=brown>bar]</font></a>baz', 'delete', 'inserttext'],
        ['[foo<font color=brown><a href=http://www.google.com>bar]</a></font>baz', 'delete', 'inserttext'],
        ['[foo<a href=http://www.google.com><font color=black>bar]</font></a>baz', 'delete', 'inserttext'],
        ['[foo<a href=http://www.google.com><u>bar]</u></a>baz', 'delete', 'inserttext'],
        ['[foo<u><a href=http://www.google.com>bar]</a></u>baz', 'delete', 'inserttext'],
        ['[foo<sub><font size=2>bar]</font></sub>baz', 'delete', 'inserttext'],
        ['[foo<font size=2><sub>bar]</sub></font>baz', 'delete', 'inserttext'],
        ['[foo<sub><font size=3>bar]</font></sub>baz', 'delete', 'inserttext'],
        ['[foo<font size=3><sub>bar]</sub></font>baz', 'delete', 'inserttext'],

        ['foo<b>[bar</b>baz]', 'delete', 'inserttext'],
        ['foo<i>[bar</i>baz]', 'delete', 'inserttext'],
        ['foo<s>[bar</s>baz]', 'delete', 'inserttext'],
        ['foo<sub>[bar</sub>baz]', 'delete', 'inserttext'],
        ['foo<sup>[bar</sup>baz]', 'delete', 'inserttext'],
        ['foo<u>[bar</u>baz]', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com>[bar</a>baz]', 'delete', 'inserttext'],
        ['foo<font face=sans-serif>[bar</font>baz]', 'delete', 'inserttext'],
        ['foo<font size=4>[bar</font>baz]', 'delete', 'inserttext'],
        ['foo<font color=#0000FF>[bar</font>baz]', 'delete', 'inserttext'],
        ['foo<span style=background-color:#00FFFF>[bar</span>baz]', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><font color=blue>[bar</font></a>baz]', 'delete', 'inserttext'],
        ['foo<font color=blue><a href=http://www.google.com>[bar</a></font>baz]', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><font color=brown>[bar</font></a>baz]', 'delete', 'inserttext'],
        ['foo<font color=brown><a href=http://www.google.com>[bar</a></font>baz]', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><font color=black>[bar</font></a>baz]', 'delete', 'inserttext'],
        ['foo<a href=http://www.google.com><u>[bar</u></a>baz]', 'delete', 'inserttext'],
        ['foo<u><a href=http://www.google.com>[bar</a></u>baz]', 'delete', 'inserttext'],
        ['foo<sub><font size=2>[bar</font></sub>baz]', 'delete', 'inserttext'],
        ['foo<font size=2><sub>[bar</sub></font>baz]', 'delete', 'inserttext'],
        ['foo<sub><font size=3>[bar</font></sub>baz]', 'delete', 'inserttext'],
        ['foo<font size=3><sub>[bar</sub></font>baz]', 'delete', 'inserttext'],

        // https://bugs.webkit.org/show_bug.cgi?id=19702
        ['<blockquote><font color=blue>[foo]</font></blockquote>', 'delete', 'inserttext'],
    ],
    //@}
};
tests.backcolor = tests.hilitecolor;
tests.insertlinebreak = tests.insertparagraph;

// Tests that start with "!" are believed to have bogus results and should be
// skipped until the relevant bugs are fixed.
var badTests = {};
(function(){
    for (var command in tests) {
        badTests[command] = [];
        for (var i = 0; i < tests[command].length; i++) {
            var test = tests[command][i];
            if (typeof test == "string" && test[0] == "!") {
                test = test.slice(1);
                tests[command][i] = test;
                badTests[command].push(test);
            }
            if (typeof test == "object" && test[0][0] == "!") {
                test = [test[0].slice(1)].concat(test.slice(1));
                tests[command][i] = test;
                badTests[command].push(test);
            }
        }
    }
})();

var defaultValues = {
//@{
    backcolor: "#00FFFF",
    createlink: "http://www.google.com/",
    fontname: "sans-serif",
    fontsize: "4",
    forecolor: "#0000FF",
    formatblock: "<div>",
    hilitecolor: "#00FFFF",
    inserthorizontalrule: "",
    inserthtml: "ab<b>c</b>d",
    insertimage: "/img/lion.svg",
    inserttext: "a",
    defaultparagraphseparator: "div",
    stylewithcss: "true",
    usecss: "true",
};
//@}

var notes = {
//@{
    fontname: 'Note that the body\'s font-family is "serif".',
};
//@}

var doubleTestingCommands = [
//@{
    "backcolor",
    "bold",
    "fontname",
    "fontsize",
    "forecolor",
    "italic",
    "justifycenter",
    "justifyfull",
    "justifyleft",
    "justifyright",
    "strikethrough",
    "stylewithcss",
    "subscript",
    "superscript",
    "underline",
    "usecss",
];
//@}

function prettyPrint(value) {
//@{
    // Partly stolen from testharness.js
    if (typeof value != "string") {
        return String(value);
    }

    value = value.replace(/\\/g, "\\\\")
        .replace(/"/g, '\\"');

    for (var i = 0; i < 32; i++) {
        var replace = "\\";
        switch (i) {
        case 0: replace += "0"; break;
        case 1: replace += "x01"; break;
        case 2: replace += "x02"; break;
        case 3: replace += "x03"; break;
        case 4: replace += "x04"; break;
        case 5: replace += "x05"; break;
        case 6: replace += "x06"; break;
        case 7: replace += "x07"; break;
        case 8: replace += "b"; break;
        case 9: replace += "t"; break;
        case 10: replace += "n"; break;
        case 11: replace += "v"; break;
        case 12: replace += "f"; break;
        case 13: replace += "r"; break;
        case 14: replace += "x0e"; break;
        case 15: replace += "x0f"; break;
        case 16: replace += "x10"; break;
        case 17: replace += "x11"; break;
        case 18: replace += "x12"; break;
        case 19: replace += "x13"; break;
        case 20: replace += "x14"; break;
        case 21: replace += "x15"; break;
        case 22: replace += "x16"; break;
        case 23: replace += "x17"; break;
        case 24: replace += "x18"; break;
        case 25: replace += "x19"; break;
        case 26: replace += "x1a"; break;
        case 27: replace += "x1b"; break;
        case 28: replace += "x1c"; break;
        case 29: replace += "x1d"; break;
        case 30: replace += "x1e"; break;
        case 31: replace += "x1f"; break;
        }
        value = value.replace(new RegExp(String.fromCharCode(i), "g"), replace);
    }
    return '"' + value + '"';
}
//@}

function doSetup(selector, idx) {
//@{
    var table = document.querySelectorAll(selector)[idx];

    var tr = document.createElement("tr");
    table.firstChild.appendChild(tr);
    tr.className = (tr.className + " active").trim();

    return tr;
}
//@}

function queryOutputHelper(beforeIndeterm, beforeState, beforeValue,
                           afterIndeterm, afterState, afterValue,
                           command, value) {
//@{
    var frag = document.createDocumentFragment();
    var beforeDiv = document.createElement("div");
    var afterDiv = document.createElement("div");
    frag.appendChild(beforeDiv);
    frag.appendChild(afterDiv);
    beforeDiv.className = afterDiv.className = "extra-results";
    beforeDiv.textContent = "Before: ";
    afterDiv.textContent = "After: ";

    beforeDiv.appendChild(document.createElement("span"));
    afterDiv.appendChild(document.createElement("span"));
    if ("indeterm" in commands[command]) {
        // We only know it has to be either true or false.
        if (beforeIndeterm !== true && beforeIndeterm !== false) {
            beforeDiv.lastChild.className = "bad-result";
        }
    } else {
        // It always has to be false.
        beforeDiv.lastChild.className = beforeIndeterm === false
            ? "good-result"
            : "bad-result";
    }
    // After running the command, indeterminate must always be false, except if
    // it's an exception, or if it's insert*list and the state was true to
    // begin with.  And we can't help strikethrough/underline.
    if ((/^insert(un)?orderedlist$/.test(command) && beforeState)
    || command == "strikethrough"
    || command == "underline") {
        if (afterIndeterm !== true && afterIndeterm !== false) {
            afterDiv.lastChild.className = "bad-result";
        }
    } else {
        afterDiv.lastChild.className =
            afterIndeterm === false
                ? "good-result"
                : "bad-result";
    }
    beforeDiv.lastChild.textContent = "indeterm " + prettyPrint(beforeIndeterm);
    afterDiv.lastChild.textContent = "indeterm " + prettyPrint(afterIndeterm);

    beforeDiv.appendChild(document.createTextNode(", "));
    afterDiv.appendChild(document.createTextNode(", "));

    beforeDiv.appendChild(document.createElement("span"));
    afterDiv.appendChild(document.createElement("span"));
    if (/^insert(un)?orderedlist$/.test(command)) {
        // If the before state is true, the after state could be either true or
        // false.  But if the before state is false, the after state has to be
        // true.
        if (beforeState !== true && beforeState !== false) {
            beforeDiv.lastChild.className = "bad-result";
        }
        if (!beforeState) {
            afterDiv.lastChild.className = afterState === true
                ? "good-result"
                : "bad-result";
        } else if (afterState !== true && afterState !== false) {
            afterDiv.lastChild.className = "bad-result";
        }
    } else if (/^justify(center|full|left|right)$/.test(command)) {
        // We don't know about the before state, but the after state is always
        // supposed to be true.
        if (beforeState !== true && beforeState !== false) {
            beforeDiv.lastChild.className = "bad-result";
        }
        afterDiv.lastChild.className = afterState === true
            ? "good-result"
            : "bad-result";
    } else if (command == "strikethrough" || command == "underline") {
        // The only thing we can say is the before/after states need to be
        // either true or false.
        if (beforeState !== true && beforeState !== false) {
            beforeDiv.lastChild.className = "bad-result";
        }
        if (afterState !== true && afterState !== false) {
            afterDiv.lastChild.className = "bad-result";
        }
    } else {
        // The general rule is it must flip the state, unless there's no state
        // defined, in which case it should always be false.
        beforeDiv.lastChild.className =
        afterDiv.lastChild.className =
            ("state" in commands[command] && typeof beforeState == "boolean" && typeof afterState == "boolean" && beforeState === !afterState)
            || (!("state" in commands[command]) && beforeState === false && afterState === false)
                ? "good-result"
                : "bad-result";
    }
    beforeDiv.lastChild.textContent = "state " + prettyPrint(beforeState);
    afterDiv.lastChild.textContent = "state " + prettyPrint(afterState);

    beforeDiv.appendChild(document.createTextNode(", "));
    afterDiv.appendChild(document.createTextNode(", "));

    beforeDiv.appendChild(document.createElement("span"));
    afterDiv.appendChild(document.createElement("span"));

    // Direct equality comparison doesn't make sense in a bunch of cases.
    if (command == "backcolor" || command == "forecolor" || command == "hilitecolor") {
        if (/^([0-9a-fA-F]{3}){1,2}$/.test(value)) {
            value = "#" + value;
        }
    } else if (command == "fontsize") {
        value = normalizeFontSize(value);
        if (value !== null) {
            value = String(cssSizeToLegacy(value));
        }
    } else if (command == "formatblock") {
        value = value.replace(/^<(.*)>$/, "$1").toLowerCase();
    } else if (command == "defaultparagraphseparator") {
        value = value.toLowerCase();
        if (value != "p" && value != "div") {
            value = "";
        }
    }

    if (((command == "backcolor" || command == "forecolor" || command == "hilitecolor") && value.toLowerCase() == "currentcolor")
    || (command == "fontsize" && value === null)
    || (command == "formatblock" && formattableBlockNames.indexOf(value.replace(/^<(.*)>$/, "$1").trim()) == -1)
    || (command == "defaultparagraphseparator" && value == "")) {
        afterDiv.lastChild.className = beforeValue === afterValue
            ? "good-result"
            : "bad-result";
    } else if (/^justify(center|full|left|right)$/.test(command)) {
        // We know there are only four correct values beforehand, and afterward
        // the value has to be the one we set.
        if (!/^(center|justify|left|right)$/.test(beforeValue)) {
            beforeDiv.lastChild.className = "bad-result";
        }
        var expectedValue = command == "justifyfull"
            ? "justify"
            : command.replace("justify", "");
        afterDiv.lastChild.className = afterValue === expectedValue
            ? "good-result"
            : "bad-result";
    } else if (!("value" in commands[command])) {
        // If it's not defined we want "".
        beforeDiv.lastChild.className = beforeValue === ""
            ? "good-result"
            : "bad-result";
        afterDiv.lastChild.className = afterValue === ""
            ? "good-result"
            : "bad-result";
    } else {
        // And in all other cases, the value afterwards has to be the one we
        // set.
        afterDiv.lastChild.className =
            areEquivalentValues(command, afterValue, value)
                ? "good-result"
                : "bad-result";
    }
    beforeDiv.lastChild.textContent = "value " + prettyPrint(beforeValue);
    afterDiv.lastChild.textContent = "value " + prettyPrint(afterValue);

    return frag;
}
//@}

function normalizeTest(command, test, styleWithCss) {
//@{
    // Our standard format for test processing is:
    //   [input HTML,
    //     [command1, value1, optional_name_mod],
    //     [command2, value2, optional_name_mod], ...]
    // Where `optional_name_mod` is an optionally-specified string used when
    // generating test names (necessary to ensure uniqueness for command
    // sequences that use the same command multiple times). This format is
    // verbose, so we actually use three different formats in the tests and
    // multiTests arrays:
    //
    // 1) Plain string giving the input HTML.  The command is implicit from the
    // key of the tests array.  If the command takes values, the value is given
    // by defaultValues, otherwise it's "".  Has to be converted to
    // [input HTML, [command, value].
    //
    // 2) Two-element array [value, input HTML].  Has to be converted to
    // [input HTML, [command, value]].
    //
    // 3) An element of multiTests.  This just has to have values filled in.
    //
    // Optionally, a styleWithCss argument can be passed, either true or false.
    // If it is, we'll prepend a styleWithCss invocation.
    if (command == "multitest") {
        if (typeof test == "string") {
            test = JSON.parse(test);
        }
        for (var i = 1; i < test.length; i++) {
            if (typeof test[i] == "string"
            && test[i] in defaultValues) {
                test[i] = [test[i], defaultValues[test[i]]];
            } else if (typeof test[i] == "string") {
                test[i] = [test[i], ""];
            }
        }
        return test;
    }

    if (typeof test == "string") {
        if (command in defaultValues) {
            test = [test, [command, defaultValues[command]]];
        } else {
            test = [test, [command, ""]];
        }
    } else if (test.length == 2) {
        test = [test[1], [command, String(test[0])]];
    }

    if (styleWithCss !== undefined) {
        test.splice(1, 0, ["stylewithcss", String(styleWithCss)]);
    }

    return test;
}
//@}

function doInputCell(tr, test, command) {
//@{
    var testHtml = test[0];

    var msg = null;
    if (command in defaultValues) {
        // Single command with a value, possibly with a styleWithCss stuck
        // before.  We don't need to specify the command itself, since this
        // presumably isn't in multiTests, so the command is already given by
        // the section header.
        msg = 'value: ' + prettyPrint(test[test.length - 1][1]);
    } else if (command == "multitest") {
        // Uses a different input format
        msg = JSON.stringify(test);
    }
    var inputCell = document.createElement("td");
    inputCell.innerHTML = "<div></div><div></div>";
    inputCell.firstChild.innerHTML = testHtml;
    inputCell.lastChild.textContent = inputCell.firstChild.innerHTML;
    if (msg !== null) {
        inputCell.lastChild.textContent += " (" + msg + ")";
    }

    tr.appendChild(inputCell);
}
//@}

function doSpecCell(tr, test, command) {
//@{
    var specCell = document.createElement("td");
    tr.appendChild(specCell);
    try {
        var points = setupCell(specCell, test[0]);
        var range = document.createRange();
        range.setStart(points[0], points[1]);
        range.setEnd(points[2], points[3]);
        // The points might be backwards
        if (range.collapsed) {
            range.setEnd(points[0], points[1]);
        }
        specCell.firstChild.contentEditable = "true";
        specCell.firstChild.spellcheck = false;

        if (command != "multitest") {
            try { var beforeIndeterm = myQueryCommandIndeterm(command, range) }
            catch(e) { beforeIndeterm = "Exception" }
            try { var beforeState = myQueryCommandState(command, range) }
            catch(e) { beforeState = "Exception" }
            try { var beforeValue = myQueryCommandValue(command, range) }
            catch(e) { beforeValue = "Exception" }
        }

        for (var i = 1; i < test.length; i++) {
            myExecCommand(test[i][0], false, test[i][1], range);
        }

        if (command != "multitest") {
            try { var afterIndeterm = myQueryCommandIndeterm(command, range) }
            catch(e) { afterIndeterm = "Exception" }
            try { var afterState = myQueryCommandState(command, range) }
            catch(e) { afterState = "Exception" }
            try { var afterValue = myQueryCommandValue(command, range) }
            catch(e) { afterValue = "Exception" }
        }

        specCell.firstChild.contentEditable = "inherit";
        specCell.firstChild.removeAttribute("spellcheck");
        var compareDiv1 = specCell.firstChild.cloneNode(true);

        // Now do various sanity checks, and throw if they're violated.  First
        // just count children:
        if (specCell.childNodes.length != 2) {
            throw "The cell didn't have two children.  Did something spill outside the test div?";
        }

        // Now verify that the DOM serializes.
        compareDiv1.normalize();
        var compareDiv2 = compareDiv1.cloneNode(false);
        compareDiv2.innerHTML = compareDiv1.innerHTML;
        // Oddly, IE9 sometimes produces two nodes that return true for
        // isEqualNode but have different innerHTML (omitting closing tags vs.
        // not).
        if (!compareDiv1.isEqualNode(compareDiv2)
        && compareDiv1.innerHTML != compareDiv2.innerHTML) {
            throw "DOM does not round-trip through serialization!  "
                + compareDiv1.innerHTML + " vs. " + compareDiv2.innerHTML;
        }
        if (!compareDiv1.isEqualNode(compareDiv2)) {
            throw "DOM does not round-trip through serialization (although innerHTML is the same)!  "
                + compareDiv1.innerHTML;
        }

        // Check for attributes
        if (specCell.firstChild.attributes.length) {
            throw "Wrapper div has attributes!  " +
                specCell.innerHTML.replace(/<div><\/div>$/, "");
        }

        // Final sanity check: make sure everything isAllowedChild() of its
        // parent.
        getDescendants(specCell.firstChild).forEach(function(descendant) {
            if (!isAllowedChild(descendant, descendant.parentNode)) {
                throw "Something here is not an allowed child of its parent: " + descendant;
            }
        });

        addBrackets(range);

        specCell.lastChild.textContent = specCell.firstChild.innerHTML;
        if (command != "multitest") {
            specCell.lastChild.appendChild(queryOutputHelper(
                beforeIndeterm, beforeState, beforeValue,
                afterIndeterm, afterState, afterValue,
                command, test[test.length - 1][1]));
            if (specCell.querySelector(".bad-result")) {
                specCell.parentNode.className = "alert";
            }
        }
    } catch (e) {
        specCell.firstChild.contentEditable = "inherit";
        specCell.firstChild.removeAttribute("spellcheck");
        specCell.lastChild.textContent = "Exception: " + formatException(e);

        specCell.parentNode.className = "alert";
        specCell.lastChild.className = "alert";

        // Don't bother comparing to localStorage, this is always wrong no
        // matter what.
        return;
    }

    if (command != "multitest") {
        // Old storage format
        var key = "execcommand-" + command
            + "-" + (test.length == 2 || test[1][1] == "false" ? "0" : "1")
            + "-" + tr.firstChild.lastChild.textContent;
    } else {
        var key = "execcommand-" + JSON.stringify(test);
    }

    // Use getItem() instead of direct property access to work around Firefox
    // bug: https://bugzilla.mozilla.org/show_bug.cgi?id=532062
    var oldValue = localStorage.getItem(key);
    var newValue = specCell.lastChild.firstChild.textContent;

    // Ignore differences between {} and [].
    if (oldValue === null
    || oldValue.replace("{}", "[]") !== newValue.replace("{}", "[]")) {
        specCell.parentNode.className = "alert";
        var alertDiv = document.createElement("div");
        specCell.lastChild.appendChild(alertDiv);
        alertDiv.className = "alert";
        if (oldValue === null) {
            alertDiv.textContent = "Newly added test result";
        } else if (oldValue.replace(/[\[\]{}]/g, "") == newValue.replace(/[\[\]{}]/g, "")) {
            alertDiv.textContent = "Last run produced a different selection: " + oldValue;
        } else {
            alertDiv.textContent = "Last run produced different markup: " + oldValue;
        }

        var button = document.createElement("button");
        alertDiv.appendChild(button);
        button.textContent = "Store new result";
        button.className = "store-new-result";
        button.onclick = (function(key, val, alertDiv) { return function() {
            localStorage[key] = val;
            // Make it easier to do mass updates, and also to jump to the next
            // new result
            var buttons = document.getElementsByClassName("store-new-result");
            for (var i = 0; i < buttons.length; i++) {
                if (isDescendant(buttons[i], alertDiv)
                && i + 1 < buttons.length) {
                    buttons[i + 1].focus();
                    break;
                }
            }
            var td = alertDiv;
            while (td.tagName != "TD") {
                td = td.parentNode;
            }
            alertDiv.parentNode.removeChild(alertDiv);
            if (!td.querySelector(".alert")) {
                td.parentNode.className = (" " + td.parentNode.className + " ")
                    .replace(/ alert /g, "")
                    .replace(/^ | $/g, "");
            }
        } })(key, newValue, alertDiv);
    }
}
//@}

function browserCellException(e, testDiv, browserCell) {
//@{
    if (testDiv) {
        testDiv.contenteditable = "inherit";
        testDiv.removeAttribute("spellcheck");
    }
    browserCell.lastChild.className = "alert";
    browserCell.lastChild.textContent = "Exception: " + formatException(e);
    if (testDiv && testDiv.parentNode != browserCell) {
        browserCell.insertBefore(testDiv, browserCell.firstChild);
    }
}
//@}

function formatException(e) {
//@{
    if (typeof e == "object" && "stack" in e) {
        return e + " (stack: " + e.stack + ")";
    }
    return String(e);
}
//@}

function doSameCell(tr) {
//@{
    tr.className = (" " + tr.className + " ").replace(" active ", "").trim();
    if (tr.className == "") {
        tr.removeAttribute("class");
    }

    var sameCell = document.createElement("td");
    if (!document.querySelector("#browser-checkbox").checked) {
        sameCell.className = "maybe";
        sameCell.textContent = "?";
    } else {
        var exception = false;
        try {
            // Ad hoc normalization to avoid basically spurious mismatches.  For
            // now this includes ignoring where the selection goes.
            var normalizedSpecCell = tr.childNodes[1].lastChild.firstChild.textContent
                .replace(/[[\]{}]/g, "")
                .replace(/ style="margin: 0 0 0 40px; border: none; padding: 0px;"/g, '')
                .replace(/ style="margin-right: 0px;" dir="ltr"/g, '')
                .replace(/ style="margin-left: 0px;" dir="rtl"/g, '')
                .replace(/ style="margin-(left|right): 40px;"/g, '')
                .replace(/: /g, ":")
                .replace(/;? ?"/g, '"')
                .replace(/<(\/?)strong/g, '<$1b')
                .replace(/<(\/?)strike/g, '<$1s')
                .replace(/<(\/?)em/g, '<$1i')
                .replace(/#[0-9a-fA-F]{6}/g, function(match) { return match.toUpperCase(); });
            var normalizedBrowserCell = tr.childNodes[2].lastChild.firstChild.textContent
                .replace(/[[\]{}]/g, "")
                .replace(/ style="margin: 0 0 0 40px; border: none; padding: 0px;"/g, '')
                .replace(/ style="margin-right: 0px;" dir="ltr"/g, '')
                .replace(/ style="margin-left: 0px;" dir="rtl"/g, '')
                .replace(/ style="margin-(left|right): 40px;"/g, '')
                .replace(/: /g, ":")
                .replace(/;? ?"/g, '"')
                .replace(/<(\/?)strong/g, '<$1b')
                .replace(/<(\/?)strike/g, '<$1s')
                .replace(/<(\/?)em/g, '<$1i')
                .replace(/#[0-9a-fA-F]{6}/g, function(match) { return match.toUpperCase(); })
                .replace(/ size="2" width="100%"/g, '');
            if (navigator.userAgent.indexOf("MSIE") != -1) {
                // IE produces <font style> instead of <span style>, so let's
                // translate all <span>s to <font>s.
                normalizedSpecCell = normalizedSpecCell
                    .replace(/<(\/?)span/g, '<$1font');
                normalizedBrowserCell = normalizedBrowserCell
                    .replace(/<(\/?)span/g, '<$1font');
            }
        } catch (e) {
            exception = true;
        }
        if (!exception && normalizedSpecCell == normalizedBrowserCell) {
            sameCell.className = "yes";
            sameCell.textContent = "\u2713";
        } else {
            sameCell.className = "no";
            sameCell.textContent = "\u2717";
        }
    }
    tr.appendChild(sameCell);

    for (var i = 0; i <= 2; i++) {
        // Insert <wbr> so IE doesn't stretch the screen.  This is considerably
        // more complicated than it has to be, thanks to Firefox's lack of
        // support for outerHTML.
        var div = tr.childNodes[i].lastChild;
        if (div.firstChild) {
            var text = div.firstChild.textContent;
            div.removeChild(div.firstChild);
            div.insertBefore(document.createElement("div"), div.firstChild);
            div.firstChild.innerHTML = text
                .replace(/&/g, "&amp;")
                .replace(/</g, "&lt;")
                .replace(/>/g, "><wbr>")
                .replace(/&lt;/g, "<wbr>&lt;");
            while (div.firstChild.hasChildNodes()) {
                div.insertBefore(div.firstChild.lastChild, div.firstChild.nextSibling);
            }
            div.removeChild(div.firstChild);
        }

        // Add position: absolute span to not affect vertical layout
        getDescendants(tr.childNodes[i].firstChild)
        .filter(function(node) {
            return node.nodeType == Node.TEXT_NODE
                && /^(\{\}?|\})$/.test(node.data);
        }).forEach(function(node) {
            var span = document.createElement("span");
            span.style.position = "absolute";
            span.textContent = node.data;
            node.parentNode.insertBefore(span, node);
            node.parentNode.removeChild(node);
        });
    }
}
//@}

function doTearDown(command) {
//@{
    getSelection().removeAllRanges();
}
//@}

function setupCell(cell, html) {
//@{
    cell.innerHTML = "<div></div><div></div>";

    return setupDiv(cell.firstChild, html);
}
//@}

function setupDiv(node, html) {
//@{
    // A variety of checks to avoid simple errors.  Not foolproof, of course.
    var re = /\{|\[|data-start/g;
    var markers = [];
    var marker;
    while (marker = re.exec(html)) {
        markers.push(marker);
    }
    if (markers.length != 1) {
        throw "Need exactly one start marker ([ or { or data-start), found " + markers.length;
    }

    var re = /\}|\]|data-end/g;
    var markers = [];
    var marker;
    while (marker = re.exec(html)) {
        markers.push(marker);
    }
    if (markers.length != 1) {
        throw "Need exactly one end marker (] or } or data-end), found " + markers.length;
    }

    node.innerHTML = html;

    var startNode, startOffset, endNode, endOffset;

    // For braces that don't lie inside text nodes, we can't just set
    // innerHTML, because that might disturb the DOM.  For instance, if the
    // brace is right before a <tr>, it could get moved outside the table
    // entirely, which messes everything up pretty badly.  So we instead
    // allow using data attributes: data-start and data-end on the start and
    // end nodes, with a numeric value indicating the offset.  This format
    // doesn't allow the parent div to be a start or end node, but in that case
    // you can always use the curly braces.
    if (node.querySelector("[data-start]")) {
        startNode = node.querySelector("[data-start]");
        startOffset = startNode.getAttribute("data-start");
        startNode.removeAttribute("data-start");
    }
    if (node.querySelector("[data-end]")) {
        endNode = node.querySelector("[data-end]");
        endOffset = endNode.getAttribute("data-end");
        endNode.removeAttribute("data-end");
    }

    var cur = node;
    while (true) {
        if (!cur || (cur != node && !(cur.compareDocumentPosition(node) & Node.DOCUMENT_POSITION_CONTAINS))) {
            break;
        }

        if (cur.nodeType != Node.TEXT_NODE) {
            cur = nextNode(cur);
            continue;
        }

        var data = cur.data.replace(/\]/g, "");
        var startIdx = data.indexOf("[");

        data = cur.data.replace(/\[/g, "");
        var endIdx = data.indexOf("]");

        cur.data = cur.data.replace(/[\[\]]/g, "");

        if (startIdx != -1) {
            startNode = cur;
            startOffset = startIdx;
        }

        if (endIdx != -1) {
            endNode = cur;
            endOffset = endIdx;
        }

        // These are only legal as the first or last
        data = cur.data.replace(/\}/g, "");
        var elStartIdx = data.indexOf("{");

        data = cur.data.replace(/\{/g, "");
        var elEndIdx = data.indexOf("}");

        if (elStartIdx == 0) {
            startNode = cur.parentNode;
            startOffset = getNodeIndex(cur);
        } else if (elStartIdx != -1) {
            startNode = cur.parentNode;
            startOffset = getNodeIndex(cur) + 1;
        }
        if (elEndIdx == 0) {
            endNode = cur.parentNode;
            endOffset = getNodeIndex(cur);
        } else if (elEndIdx != -1) {
            endNode = cur.parentNode;
            endOffset = getNodeIndex(cur) + 1;
        }

        cur.data = cur.data.replace(/[{}]/g, "");
        if (!cur.data.length) {
            if (cur == startNode || cur == endNode) {
                throw "You put a square bracket where there was no text node . . .";
            }
            var oldCur = cur;
            cur = nextNode(cur);
            oldCur.parentNode.removeChild(oldCur);
        } else {
            cur = nextNode(cur);
        }
    }

    return [startNode, startOffset, endNode, endOffset];
}
//@}

function setSelection(startNode, startOffset, endNode, endOffset) {
//@{
    if (navigator.userAgent.indexOf("Opera") != -1) {
        // Yes, browser sniffing is evil, but I can't be bothered to debug
        // Opera.
        var range = document.createRange();
        range.setStart(startNode, startOffset);
        range.setEnd(endNode, endOffset);
        if (range.collapsed) {
            range.setEnd(startNode, startOffset);
        }
        getSelection().removeAllRanges();
        getSelection().addRange(range);
    } else if ("extend" in getSelection()) {
        // WebKit behaves unreasonably for collapse(), so do that manually.
        /*
        var range = document.createRange();
        range.setStart(startNode, startOffset);
        getSelection().removeAllRanges();
        getSelection().addRange(range);
        */
        getSelection().collapse(startNode, startOffset);
        getSelection().extend(endNode, endOffset);
    } else {
        // IE9.  Selections have no direction, so we just make the selection
        // always forwards.
        var range;
        if (getSelection().rangeCount) {
            range = getSelection().getRangeAt(0);
        } else {
            range = document.createRange();
        }
        range.setStart(startNode, startOffset);
        range.setEnd(endNode, endOffset);
        if (range.collapsed) {
            // Phooey, we got them backwards.
            range.setEnd(startNode, startOffset);
        }
        if (!getSelection().rangeCount) {
            getSelection().addRange(range);
        }
    }
}
//@}

/**
 * Add brackets at the start and end points of the given range, so that they're
 * visible.
 */
function addBrackets(range) {
//@{
    // Handle the collapsed case specially, to avoid confusingly getting the
    // markers backwards in some cases
    if (range.startContainer.nodeType == Node.TEXT_NODE
    || range.startContainer.nodeType == Node.COMMENT_NODE) {
        if (range.collapsed) {
            range.startContainer.insertData(range.startOffset, "[]");
        } else {
            range.startContainer.insertData(range.startOffset, "[");
        }
    } else {
        var marker = range.collapsed ? "{}" : "{";
        if (range.startOffset != range.startContainer.childNodes.length
        && range.startContainer.childNodes[range.startOffset].nodeType == Node.TEXT_NODE) {
            range.startContainer.childNodes[range.startOffset].insertData(0, marker);
        } else if (range.startOffset != 0
        && range.startContainer.childNodes[range.startOffset - 1].nodeType == Node.TEXT_NODE) {
            range.startContainer.childNodes[range.startOffset - 1].appendData(marker);
        } else {
            // Seems to serialize as I'd want even for tables . . . IE doesn't
            // allow undefined to be passed as the second argument (it throws
            // an exception), so we have to explicitly check the number of
            // children and pass null.
            range.startContainer.insertBefore(document.createTextNode(marker),
                range.startContainer.childNodes.length == range.startOffset
                ? null
                : range.startContainer.childNodes[range.startOffset]);
        }
    }
    if (range.collapsed) {
        return;
    }
    if (range.endContainer.nodeType == Node.TEXT_NODE
    || range.endContainer.nodeType == Node.COMMENT_NODE) {
        range.endContainer.insertData(range.endOffset, "]");
    } else {
        if (range.endOffset != range.endContainer.childNodes.length
        && range.endContainer.childNodes[range.endOffset].nodeType == Node.TEXT_NODE) {
            range.endContainer.childNodes[range.endOffset].insertData(0, "}");
        } else if (range.endOffset != 0
        && range.endContainer.childNodes[range.endOffset - 1].nodeType == Node.TEXT_NODE) {
            range.endContainer.childNodes[range.endOffset - 1].appendData("}");
        } else {
            range.endContainer.insertBefore(document.createTextNode("}"),
                range.endContainer.childNodes.length == range.endOffset
                ? null
                : range.endContainer.childNodes[range.endOffset]);
        }
    }
}
//@}

function normalizeSerializedStyle(wrapper) {
//@{
    // Inline CSS attribute serialization has terrible interop, so we fix
    // things up a bit to avoid spurious mismatches.  This needs to be removed
    // once CSSOM defines this stuff properly, but for now there's just no
    // standard for any of it.  This only normalizes descendants of wrapper,
    // not wrapper itself.
    [].forEach.call(wrapper.querySelectorAll("[style]"), function(node) {
        if (node.style.color != "") {
            var newColor = normalizeColor(node.style.color);
            node.style.color = "";
            node.style.color = newColor;
        }
        if (node.style.backgroundColor != "") {
            var newBackgroundColor = normalizeColor(node.style.backgroundColor);
            node.style.backgroundColor = "";
            node.style.backgroundColor = newBackgroundColor;
        }
        node.setAttribute("style", node.getAttribute("style")
            // Random spacing differences
            .replace(/; ?$/, "")
            .replace(/: /g, ":")
            // Gecko likes "transparent"
            .replace(/transparent/g, "rgba(0, 0, 0, 0)")
            // WebKit likes to look overly precise
            .replace(/, 0.496094\)/g, ", 0.5)")
            // Gecko converts anything with full alpha to "transparent" which
            // then becomes "rgba(0, 0, 0, 0)", so we have to make other
            // browsers match
            .replace(/rgba\([0-9]+, [0-9]+, [0-9]+, 0\)/g, "rgba(0, 0, 0, 0)")
        );
    });
}
//@}

/**
 * Input is in the following format:
 *   [input HTML,
 *    array of commands,
 *    expected output HTML,
 *    array of expected execCommand() return values,
 *    object of expected indeterm/state/value].
 * The array of commands is [[command, value, optionalDesc], [command, value,
 * optionalDesc], ...].  optionalDesc is appended to the description of the
 * test in the generated test name.
 *
 * The
 * array of expected execCommand() return values is [true|false, true|false,
 * ...], where the indices match those in the array of commands.  The
 * indeterm/state/value object is of the form
 *   {command: [expected indeterm before, expected state before,
 *     expected value before, expected indeterm after,
 *     expected state after, expected value after],
 *   command: ... }
 * null for any of the last six entries means an INVALID_ACCESS_ERR must be
 * raised.
 */
function runConformanceTest(browserTest) {
//@{
    document.getElementById("test-container").innerHTML = "<div contenteditable></div><p>test";
    var testName = JSON.stringify(browserTest[1]) + " " + format_value(browserTest[0]);
    var testDiv = document.querySelector("div[contenteditable]");
    var originalRootElement, newRootElement;
    var exception = null;
    var expectedExecCommandReturnValues = browserTest[3];
    var expectedQueryResults = browserTest[4];
    var actualQueryResults = {};
    var actualQueryExceptions = {};
    var subtestName;

    try {
        var points = setupDiv(testDiv, browserTest[0]);

        var range = document.createRange();
        range.setStart(points[0], points[1]);
        range.setEnd(points[2], points[3]);
        // The points might be backwards
        if (range.collapsed) {
            range.setEnd(points[0], points[1]);
        }
        getSelection().removeAllRanges();
        getSelection().addRange(range);

        var originalRootElement = document.documentElement.cloneNode(true);
        originalRootElement.querySelector("[contenteditable]").parentNode
            .removeChild(originalRootElement.querySelector("[contenteditable]"));
        originalRootElement.querySelector("#log").parentNode
            .removeChild(originalRootElement.querySelector("#log"));

        for (var command in expectedQueryResults) {
            var results = [];
            var exceptions = {};
            try { results[0] = document.queryCommandIndeterm(command) }
            catch(e) { exceptions[0] = e }
            try { results[1] = document.queryCommandState(command) }
            catch(e) { exceptions[1] = e }
            try { results[2] = document.queryCommandValue(command) }
            catch(e) { exceptions[2] = e }
            actualQueryResults[command] = results;
            actualQueryExceptions[command] = exceptions;
        }
    } catch(e) {
        exception = e;
    }

    for (var i = 0; i < browserTest[1].length; i++) {
        subtestName = testName + ": execCommand(" +
            format_value(browserTest[1][i][0]) + ", false, " +
            format_value(browserTest[1][i][1]) + ") " +
            (browserTest[1][i][2] ? browserTest[1][i][2] + " " : "") +
            "return value"
        subsetTest(test, function() {
            assert_equals(exception, null, "Setup must not throw an exception");

            assert_equals(document.execCommand(browserTest[1][i][0], false, browserTest[1][i][1]),
                expectedExecCommandReturnValues[i]);
        }, subtestName);
    }

    if (exception === null) {
        try {
            for (var command in expectedQueryResults) {
                var results = actualQueryResults[command];
                var exceptions = actualQueryExceptions[command];
                try { results[3] = document.queryCommandIndeterm(command) }
                catch(e) { exceptions[3] = e }
                try { results[4] = document.queryCommandState(command) }
                catch(e) { exceptions[4] = e }
                try { results[5] = document.queryCommandValue(command) }
                catch(e) { exceptions[5] = e }
            }

            var newRootElement = document.documentElement.cloneNode(true);
            newRootElement.querySelector("[contenteditable]").parentNode
                .removeChild(newRootElement.querySelector("[contenteditable]"));
            newRootElement.querySelector("#log").parentNode
                .removeChild(newRootElement.querySelector("#log"));

            normalizeSerializedStyle(testDiv);
        } catch(e) {
            exception = e;
        }
    }

    subsetTest(test, function() {
        assert_equals(exception, null, "Setup must not throw an exception");

        // Now test for modifications to non-editable content.  First just
        // count children:
        assert_equals(testDiv.parentNode.childNodes.length, 2,
            "The parent div must have two children.  Did something spill outside the test div?");

        // Check for attributes
        assert_equals(testDiv.attributes.length, 1,
            'Wrapper div must have only one attribute (<div contenteditable="">), but has more (' +
            formatStartTag(testDiv) + ")");

        assert_equals(document.body.attributes.length, 0,
            "Body element must have no attributes (<body>), but has more (" +
            formatStartTag(document.body) + ")");

        // Check that in general, nothing outside the test div was modified.
        // TODO: Less verbose error reporting, the way some of the range tests
        // do?
        assert_equals(newRootElement.innerHTML, originalRootElement.innerHTML,
            "Everything outside the editable div must be unchanged, but some change did occur");
    }, testName + " checks for modifications to non-editable content");

    subsetTest(test, function() {
        assert_equals(exception, null, "Setup must not throw an exception");

        if (Array.isArray(browserTest[2])) {
          var expectedInnerHTMLArray = [];
          browserTest[2].forEach(function (expectedInnerHTML) {
            expectedInnerHTMLArray.push(expectedInnerHTML.replace(/[\[\]{}]/g, ""));
          });
          assert_in_array(testDiv.innerHTML,
               expectedInnerHTMLArray,
               "Unexpected innerHTML (after normalizing inline style)");
        } else {
          assert_equals(testDiv.innerHTML,
              browserTest[2].replace(/[\[\]{}]/g, ""),
              "Unexpected innerHTML (after normalizing inline style)");
        }
    }, testName + " compare innerHTML");

    for (var command in expectedQueryResults) {
        var descriptions = [
            'queryCommandIndeterm("' + command + '") before',
            'queryCommandState("' + command + '") before',
            'queryCommandValue("' + command + '") before',
            'queryCommandIndeterm("' + command + '") after',
            'queryCommandState("' + command + '") after',
            'queryCommandValue("' + command + '") after',
        ];
        for (var i = 0; i < 6; i++) {
            subsetTest(test, function() {
                assert_equals(exception, null, "Setup must not throw an exception");

                if (expectedQueryResults[command][i] === null) {
                    // Some ad hoc tests to verify that we have a real
                    // DOMException.  FIXME: This should be made more rigorous,
                    // with clear steps specified for checking that something
                    // is really a DOMException.
                    assert_true(i in actualQueryExceptions[command],
                        "An exception must be thrown in this case");
                    var e = actualQueryExceptions[command][i];
                    assert_equals(typeof e, "object",
                        "typeof thrown object");
                    assert_idl_attribute(e, "code",
                        "Thrown object must be a DOMException");
                    assert_idl_attribute(e, "INVALID_ACCESS_ERR",
                        "Thrown object must be a DOMException");
                    assert_equals(e.code, e.INVALID_ACCESS_ERR,
                        "Thrown object must be an INVALID_ACCESS_ERR, so its .code and .INVALID_ACCESS_ERR attributes must be equal");
                } else if ((i == 2 || i == 5)
                && (command == "backcolor" || command == "forecolor" || command == "hilitecolor")
                && typeof actualQueryResults[command][i] == "string") {
                    assert_false(i in actualQueryExceptions[command],
                        "An exception must not be thrown in this case");
                    // We don't return the format that the color should be in:
                    // that's up to CSSOM.  Thus we normalize before comparing.
                    assert_equals(normalizeColor(actualQueryResults[command][i]),
                        expectedQueryResults[command][i],
                        "Wrong result returned (after color normalization)");
                } else {
                    assert_false(i in actualQueryExceptions[command],
                        "An exception must not be thrown in this case");
                    assert_equals(actualQueryResults[command][i],
                        expectedQueryResults[command][i],
                        "Wrong result returned");
                }
            }, testName + " " + descriptions[i]);
        }
    }

    // Silly Firefox
    document.body.removeAttribute("bgcolor");
}
//@}

/**
 * Return a string like '<body bgcolor="#FFFFFF">'.
 */
function formatStartTag(el) {
//@{
    var ret = "<" + el.tagName.toLowerCase();
    for (var i = 0; i < el.attributes.length; i++) {
        ret += " " + el.attributes[i].name + '="';
        ret += el.attributes[i].value.replace(/\&/g, "&amp;")
            .replace(/"/g, "&quot;");
        ret += '"';
    }
    return ret + ">";
}
//@}

// vim: foldmarker=@{,@} foldmethod=marker
