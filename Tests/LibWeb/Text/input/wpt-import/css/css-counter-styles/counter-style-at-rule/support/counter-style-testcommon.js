function test_counter_style_descriptor(descriptor, value, expected) {
  let descriptors = [];
  descriptors.push(`${descriptor}: ${value}`);

  // Fill out the remaining necessary descriptors
  if (descriptor === 'system') {
    if (value === 'additive')
      descriptors.push('additive-symbols: 1 "I"');
    else if (!value.startsWith('extends'))
      descriptors.push('symbols: "X" "Y"');
  } else if (descriptor === 'symbols') {
    descriptors.push('system: symbolic');
  } else if (descriptor === 'additive-symbols') {
    descriptors.push('system: additive');
  } else {
    descriptors.push('system: symbolic');
    descriptors.push('symbols: "X" "Y"');
  }

  let style = document.createElement('style');
  style.textContent = `@counter-style foo { ${descriptors.join(';')} }`;
  document.head.appendChild(style);

  test(() => {
    let rule = style.sheet.cssRules[0];
    // TODO: The spec is inconsistent on when the entire rule is invalid
    // (and hence absent from OM), and when only the descriptor is invalid.
    // Revise when spec issue is resolved.
    // See https://github.com/w3c/csswg-drafts/issues/5717
    if (!rule) {
      assert_equals(expected, undefined);
      return;
    }

    assert_equals(rule.constructor.name, 'CSSCounterStyleRule');

    let text = rule.cssText;
    if (expected)
      assert_not_equals(text.indexOf(`${descriptor}: ${expected}`), -1);
    else
      assert_equals(text.indexOf(`${descriptor}:`), -1);
  }, `@counter-style '${descriptor}: ${value}' is ${expected ? 'valid' : 'invalid'}`);

  style.remove();
}

function test_valid_counter_style_descriptor(descriptor, value, expected) {
  expected = expected || value;
  test_counter_style_descriptor(descriptor, value, expected);
}

function test_invalid_counter_style_descriptor(descriptor, value) {
  test_counter_style_descriptor(descriptor, value, undefined);
}

function test_counter_style_name(name, isValid) {
  let style = document.createElement('style');
  style.textContent = `@counter-style ${name} { system: symbolic; symbols: 'X' 'Y'; }`;
  document.head.appendChild(style);

  test(() => {
    let rule = style.sheet.cssRules[0];
    if (!isValid) {
      assert_equals(rule, undefined);
      return;
    }

    assert_not_equals(rule, undefined);
    assert_equals(rule.constructor.name, 'CSSCounterStyleRule');
    assert_equals(rule.name, name);
  }, `@counter-style name ${name} is ${isValid ? 'valid' : 'invalid'}`);

  style.remove();
}

function test_valid_name(name) {
  test_counter_style_name(name, true);
}

function test_invalid_name(name) {
  test_counter_style_name(name, false);
}

