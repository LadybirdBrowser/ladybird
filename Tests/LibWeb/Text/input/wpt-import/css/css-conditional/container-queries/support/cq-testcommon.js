function assert_implements_size_container_queries() {
  assert_implements(CSS.supports("container-type:size"),
                    "Basic support for size container queries required");
}

function assert_implements_scroll_state_container_queries() {
  assert_implements(CSS.supports("container-type:scroll-state"),
                    "Basic support for scroll-state container queries required");
}

function assert_implements_style_container_queries() {
  // TODO: Replace with CSS.supports() when/if this can be expressed with at-rule().
  const sheet = new CSSStyleSheet();
  // No support means the style() function is <general-enclosed> which should
  // affect serialization. Although serialization for <general-enclosed> is not
  // specified[1], unknown function names are unlikely to be resolved to be
  // serialized lower-case. Also, keeping the case is currently interoperable.
  //
  // [1] https://github.com/w3c/csswg-drafts/issues/7266
  sheet.replaceSync('@container STYLE(--foo: bar){}');
  assert_implements(sheet.cssRules[0].containerQuery === "style(--foo: bar)",
                    "Basic support for style container queries required");
}

function cleanup_container_query_main() {
  const main = document.querySelector("#cq-main");
  while (main.firstChild)
    main.firstChild.remove();
}

function set_container_query_style(text) {
  let style = document.createElement('style');
  style.innerText = text;
  document.querySelector("#cq-main").append(style);
  return style;
}

function test_cq_rule_invalid(query) {
  const ruleText = `@container ${query} {}`;
  test(t => {
    t.add_cleanup(cleanup_container_query_main);
    let style = set_container_query_style(ruleText);
    assert_equals(style.sheet.rules.length, 0);
  }, `@container rule should be invalid: ${ruleText} {}`);
}

function test_cq_rule_valid(query) {
  const ruleText = `@container ${query} {}`;
  test(t => {
    t.add_cleanup(cleanup_container_query_main);
    let style = set_container_query_style(`@container ${query} {}`);
    assert_equals(style.sheet.rules.length, 1);
  }, `@container rule should be valid: ${ruleText} {}`);
}

function test_cq_condition_invalid(condition) {
  test(t => {
    t.add_cleanup(cleanup_container_query_main);
    let style = set_container_query_style(`@container name ${condition} {}`);
    assert_equals(style.sheet.rules.length, 0);
  }, `Query condition should be invalid: ${condition}`);
}

// Tests that 1) the condition parses, and 2) is either "unknown" or not, as
// specified.
function test_cq_condition_valid(condition, unknown) {
  test(t => {
    t.add_cleanup(cleanup_container_query_main);
    let style = set_container_query_style(`
      @container name ${condition} {}
      @container name (${condition}) or (not (${condition})) { main { --match:true; } }
    `);
    assert_equals(style.sheet.rules.length, 2);
    const expected = unknown ? '' : 'true';
    assert_equals(getComputedStyle(document.querySelector("#cq-main")).getPropertyValue('--match'), expected);
  }, `Query condition should be valid${unknown ? ' but unknown' : ''}: ${condition}`);
}

function test_cq_condition_known(condition) {
  test_cq_condition_valid(condition, false /* unknown */);
}

function test_cq_condition_unknown(condition) {
  test_cq_condition_valid(condition, true /* unknown */);
}

function test_container_name_invalid(container_name) {
  test(t => {
    t.add_cleanup(cleanup_container_query_main);
    let style = set_container_query_style(`@container ${container_name} not (width) {}`);
    assert_equals(style.sheet.rules.length, 0);
  }, `Container name: ${container_name}`);
}

function test_container_name_valid(container_name) {
  test(t => {
    t.add_cleanup(cleanup_container_query_main);
    let style = set_container_query_style(`@container ${container_name} not (width) {}`);
    assert_equals(style.sheet.rules.length, 1);
  }, `Container name: ${container_name}`);
}

function test_cq_condition_serialization(query, expected) {
  test(t => {
    t.add_cleanup(cleanup_container_query_main);
    let style = set_container_query_style(`@container ${query} {}`);
    assert_equals(style.sheet.rules.length, 1);
    assert_equals(style.sheet.rules[0].conditionText, expected);
  }, `@container conditionText serialization: ${query}`);
}
