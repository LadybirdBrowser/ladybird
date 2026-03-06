// Looks for <template> elements, and runs each test in turn, for example:
//
//  <template data-name="Example test">
//    <style>
//      @function --f() returns <length> {
//        result: calc(1px + 1px);
//      }
//      #target {
//        --actual: --f();
//        --expected: 2px;
//      }
//    </style>
//  </template>
//
// The test passes if the computed value of --actual matches
// the computed value of --expected on #target.
//
// Elements <div id=target> and <div=main> are assumed to exist.
function test_all_templates() {
  let templates = document.querySelectorAll('template');
  for (let template of templates) {
    test((t) => {
      t.add_cleanup(() => main.replaceChildren());
      main.append(template.content.cloneNode(true));
      let cs = getComputedStyle(target);
      let actual = cs.getPropertyValue('--actual');
      let expected = cs.getPropertyValue('--expected');
      assert_equals(actual, expected);
    }, template.getAttribute('data-name'));
  }
}

// Within an array of elements, find an element with id=target (recursively,
// including shadows).
function find_target(elements) {
  for (let e of (elements ?? [])) {
    let t = e.id == 'target' ? e : null;
    t ??= find_target(e.children);
    t ??= find_target(e.shadowRoot?.children);
    if (t) {
      return t;
    }
  }
  return null;
}

// Similar to test_all_templates(), but treats each <div data-name="...">
// as a test, and lets ShadowDOM do the "inflation".
function test_all_shadows() {
  let hosts = document.querySelectorAll('div[data-name]');
  for (let host of hosts) {
    test((t) => {
      let target = find_target([host]);
      assert_true(target != null);
      let cs = getComputedStyle(target);
      let actual = cs.getPropertyValue('--actual');
      let expected = cs.getPropertyValue('--expected');
      assert_equals(actual, expected);
    }, host.getAttribute('data-name'));
  }
}