'use strict';

function query_is_css_parseable(query) {
  const style = document.createElement('style');
  style.type = 'text/css';
  document.head.appendChild(style);

  const sheet = style.sheet;
  try {
    sheet.insertRule("@media " + query + "{}", 0);
    return sheet.cssRules.length == 1 &&
        sheet.cssRules[0].media.mediaText != "not all";
  } finally {
    while (sheet.cssRules.length)
      sheet.deleteRule(0);
    style.remove();
  }
}

function query_should_be_css_parseable(query) {
  test(() => {
    assert_true(query_is_css_parseable(query));
  }, "Should be parseable in a CSS stylesheet: '" + query + "'");
}

function query_should_not_be_css_parseable(query) {
  test(() => {
    assert_false(query_is_css_parseable(query));
  }, "Should not be parseable in a CSS stylesheet: '" + query + "'");
}

function query_is_js_parseable(query) {
  // We cannot rely on whether a given feature is on or off, so only check the
  // 'media' member of the result.
  const match = window.matchMedia(query);
  return match.media == query;
}

function query_should_be_js_parseable(query) {
  test(() => {
    assert_true(query_is_js_parseable(query));
  }, "Should be parseable in JS: '" + query + "'");
}

function query_should_not_be_js_parseable(query) {
  test(() => {
    assert_false(query_is_js_parseable(query));
  }, "Should not be parseable in JS: '" + query + "'");
}

function query_is_known(query) {
  return window.matchMedia(`${query}, not all and ${query}`).matches;
}

function query_is_unknown(query) {
  return !window.matchMedia(`${query}, not all and ${query}`).matches;
}

function query_should_be_known(query) {
  test(() => {
    assert_true(query_is_js_parseable(query), "Can parse with JS");
    assert_true(query_is_css_parseable(query), "Can parse with CSS");
    assert_true(query_is_known(query));
  }, "Should be known: '" + query + "'");
}

function query_should_be_unknown(query) {
  test(() => {
    assert_true(query_is_js_parseable(query), "Can parse with JS");
    assert_true(query_is_css_parseable(query), "Can parse with CSS");
  }, "Should be parseable: '" + query + "'");

  test(() => {
    assert_true(query_is_unknown(query));
  }, "Should be unknown: '" + query + "'");
}
