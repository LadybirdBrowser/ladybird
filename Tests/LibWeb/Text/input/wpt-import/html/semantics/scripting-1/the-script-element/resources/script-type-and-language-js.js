function testAttribute(attr, val, shouldRun) {
  test(function() {
    assert_false(window.ran, "ran variable not reset");
    let script;
    if (document.contentType === 'image/svg+xml') {
      // SVG
      script = document.createElementNS("http://www.w3.org/2000/svg", "script");
    } else {
      // HTML or XHTML
      script = document.createElement("script");
    }
    script.setAttribute(attr, val);
    script.textContent = "window.ran = true;";
    document.querySelector('#script-placeholder').appendChild(script);
    assert_equals(window.ran, shouldRun);
  }, "Script should" + (shouldRun ? "" : "n't") + " run with " + attr + "=" +
     format_value(val));
  window.ran = false;
}
function testTypeShouldRun(type) {
  testAttribute("type", type, true);
}
function testLanguageShouldRun(lang) {
  testAttribute("language", lang, true);
}
function testTypeShouldNotRun(type) {
  testAttribute("type", type, false);
}
function testLanguageShouldNotRunUnlessSVG(lang) {
  // In SVGs, there is no concrete spec but all browsers agree that
  // language attributes have no effects and thus script elements
  // without type attributes are always expected to run regardless of
  // language attributes.
  const expectedToRun = document.contentType === 'image/svg+xml';
  testAttribute("language", lang, expectedToRun);
}

// Unlike `test*()` methods above, there should be a (parser-inserted) script
// with an invalid type/language that would set `window.ran` to true just
// before `testParserInsertedDidNotRun()`, and
// `testParserInsertedDidNotRun()` asserts that the script did not run.
// `window.ran` should be reset where needed. For example:
//   <script>window.ran = false;</script>
//   <script type="invalid-type">window.ran = true;</script>
//   <script>testParserInsertedDidNotRun('type=invalid-type');</script>
function testParserInsertedDidNotRun(description) {
  test(() => assert_false(window.ran),
       "Script shouldn't run with " + description + " (parser-inserted)");
  window.ran = false;
}

// When prefixed by "application/", these match with
// https://mimesniff.spec.whatwg.org/#javascript-mime-type
const application = [
  "ecmascript",
  "javascript",
  "x-ecmascript",
  "x-javascript"
];

// When prefixed by "text/", these match with
// https://mimesniff.spec.whatwg.org/#javascript-mime-type
const text = [
  "ecmascript",
  "javascript",
  "javascript1.0",
  "javascript1.1",
  "javascript1.2",
  "javascript1.3",
  "javascript1.4",
  "javascript1.5",
  "jscript",
  "livescript",
  "x-ecmascript",
  "x-javascript"
];

const legacyTypes = [
  "javascript1.6",
  "javascript1.7",
  "javascript1.8",
  "javascript1.9"
];

const spaces = [" ", "\t", "\n", "\r", "\f"];

window.ran = false;

// Type attribute

testTypeShouldRun("");
testTypeShouldNotRun(" ");

application.map(t => "application/" + t).forEach(testTypeShouldRun);
application.map(t => ("application/" + t).toUpperCase()).forEach(
    testTypeShouldRun);

spaces.forEach(function(s) {
  application.map(t => "application/" + t + s).forEach(testTypeShouldRun);
  application.map(t => s + "application/" + t).forEach(testTypeShouldRun);
});

application.map(t => "application/" + t + "\0").forEach(testTypeShouldNotRun);
application.map(t => "application/" + t + "\0foo").forEach(
    testTypeShouldNotRun);

text.map(t => "text/" + t).forEach(testTypeShouldRun);
text.map(t => ("text/" + t).toUpperCase()).forEach(testTypeShouldRun);

legacyTypes.map(t => "text/" + t).forEach(testTypeShouldNotRun);

spaces.forEach(function(s) {
  text.map(t => "text/" + t + s).forEach(testTypeShouldRun);
  text.map(t => s + "text/" + t).forEach(testTypeShouldRun);
});

text.map(t => "text/" + t + "\0").forEach(testTypeShouldNotRun);
text.map(t => "text/" + t + "\0foo").forEach(testTypeShouldNotRun);

text.forEach(testTypeShouldNotRun);
legacyTypes.forEach(testTypeShouldNotRun);

// Language attribute

testLanguageShouldRun("");
testLanguageShouldNotRunUnlessSVG(" ");

text.forEach(testLanguageShouldRun);
text.map(t => t.toUpperCase()).forEach(testLanguageShouldRun);

legacyTypes.forEach(testLanguageShouldNotRunUnlessSVG);

spaces.forEach(function(s) {
  text.map(t => t + s).forEach(testLanguageShouldNotRunUnlessSVG);
  text.map(t => s + t).forEach(testLanguageShouldNotRunUnlessSVG);
});
text.map(t => t + "xyz").forEach(testLanguageShouldNotRunUnlessSVG);
text.map(t => "xyz" + t).forEach(testLanguageShouldNotRunUnlessSVG);

text.map(t => t + "\0").forEach(testLanguageShouldNotRunUnlessSVG);
text.map(t => t + "\0foo").forEach(testLanguageShouldNotRunUnlessSVG);
