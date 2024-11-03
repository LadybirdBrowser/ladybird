testText("<div>", "abc", "abc", "Simplest possible test");
testHTML("<div>", "abc\ndef", "abc<br>def", "Newlines convert to <br> in non-white-space:pre elements");
testHTML("<pre>", "abc\ndef", "abc<br>def", "Newlines convert to <br> in <pre> element");
testHTML("<textarea>", "abc\ndef", "abc<br>def", "Newlines convert to <br> in <textarea> element");
testHTML("<div style='white-space:pre'>", "abc\ndef", "abc<br>def", "Newlines convert to <br> in white-space:pre element");
testHTML("<div>", "abc\rdef", "abc<br>def", "CRs convert to <br> in non-white-space:pre elements");
testHTML("<pre>", "abc\rdef", "abc<br>def", "CRs convert to <br> in <pre> element");
testHTML("<div>", "abc\r\ndef", "abc<br>def", "Newline/CR pair converts to <br> in non-white-space:pre element");
testHTML("<div>", "abc\n\ndef", "abc<br><br>def", "Newline/newline pair converts to two <br>s in non-white-space:pre element");
testHTML("<div>", "abc\r\rdef", "abc<br><br>def", "CR/CR pair converts to two <br>s in non-white-space:pre element");
testHTML("<div style='white-space:pre'>", "abc\rdef", "abc<br>def", "CRs convert to <br> in white-space:pre element");
testText("<div>", "abc<def", "abc<def", "< preserved");
testText("<div>", "abc>def", "abc>def", "> preserved");
testText("<div>", "abc&", "abc&", "& preserved");
testText("<div>", "abc\"def", "abc\"def", "\" preserved");
testText("<div>", "abc\'def", "abc\'def", "\' preserved");
testHTML("<svg>", "abc", "", "innerText not supported on SVG elements");
testHTML("<math>", "abc", "", "innerText not supported on MathML elements");
testText("<div>", "abc\0def", "abc\0def", "Null characters preserved");
testText("<div>", "abc\tdef", "abc\tdef", "Tabs preserved");
testText("<div>", " abc", " abc", "Leading whitespace preserved");
testText("<div>", "abc ", "abc ", "Trailing whitespace preserved");
testText("<div>", "abc  def", "abc  def", "Whitespace not compressed");
testText("<div>abc\n\n", "abc", "abc", "Existing text deleted");
testText("<div><br>", "abc", "abc", "Existing <br> deleted");
testHTML("<div>", "", "", "Assigning the empty string");
testHTML("<div>", null, "", "Assigning null");
testHTML("<div>", undefined, "undefined", "Assigning undefined");
testHTML("<div>", "\rabc", "<br>abc", "Start with CR");
testHTML("<div>", "\nabc", "<br>abc", "Start with LF");
testHTML("<div>", "\r\nabc", "<br>abc", "Start with CRLF");
testHTML("<div>", "abc\r", "abc<br>", "End with CR");
testHTML("<div>", "abc\n", "abc<br>", "End with LF");
testHTML("<div>", "abc\r\n", "abc<br>", "End with CRLF");

// Setting innerText on these should not throw
["area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr",
"image", "img", "input", "keygen", "link", "menuitem", "meta", "param",
"source", "track", "wbr", "colgroup", "frameset", "head", "html", "table",
"tbody", "tfoot", "thead", "tr"].forEach(function(tag) {
  testText(document.createElement(tag), "abc", "abc", "innerText on <" + tag + "> element");
});
