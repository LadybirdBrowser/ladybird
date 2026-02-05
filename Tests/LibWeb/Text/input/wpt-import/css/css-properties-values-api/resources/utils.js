let next_property_id = 1;

// Generate a unique property name on the form --prop-N.
function generate_name() {
  return `--prop-${next_property_id++}`;
}

// Produce a compatible initial value for the specified syntax.
function any_initial_value(syntax) {
  let components = syntax.split('|').map(x => x.trim())
  let first_component = components[0];

  if (first_component.endsWith('+') || first_component.endsWith('#'))
    first_component = first_component.slice(0, -1);

  switch (first_component) {
    case '*':
    case '<custom-ident>':
      return 'NULL';
    case '<angle>':
      return '0deg';
    case '<color>':
      return 'rgb(0, 0, 0)';
    case '<image>':
    case '<url>':
      return 'url(0)';
    case '<integer>':
    case '<length-percentage>':
    case '<length>':
    case '<number>':
      return '0';
    case '<percentage>':
      return '0%';
    case '<resolution>':
      return '0dpi';
    case '<time>':
      return '0s';
    case '<transform-function>':
    case '<transform-list>':
      return 'matrix(0, 0, 0, 0, 0, 0)';
    default:
      // We assume syntax is a specific custom ident.
      return first_component;
  }
}

// Registers a unique property on the form '--prop-N' and returns the name.
// Any value except 'syntax' may be omitted, in which case the property will
// not inherit, and some undefined (but compatible) initial value will be
// generated. If a single string is used as the argument, it is assumed to be
// the syntax.
function generate_property(reg) {
  // Verify that only valid keys are specified. This prevents the caller from
  // accidentally supplying 'inherited' instead of 'inherits', for example.
  if (typeof(reg) === 'object') {
    const permitted = new Set(['name', 'syntax', 'initialValue', 'inherits']);
    if (!Object.keys(reg).every(k => permitted.has(k)))
      throw new Error('generate_property: invalid parameter');
  }

  let syntax = typeof(reg) === 'string' ? reg : reg.syntax;
  let initial = typeof(reg.initialValue) === 'undefined' ? any_initial_value(syntax)
                                                         : reg.initialValue;
  let inherits = typeof(reg.inherits) === 'undefined' ? false : reg.inherits;

  let name = generate_name();
  CSS.registerProperty({
    name: name,
    syntax: syntax,
    initialValue: initial,
    inherits: inherits
  });
  return name;
}

function all_syntaxes() {
  return [
    '*',
    '<angle>',
    '<color>',
    '<custom-ident>',
    '<image>',
    '<integer>',
    '<length-percentage>',
    '<length>',
    '<number>',
    '<percentage>',
    '<resolution>',
    '<time>',
    '<transform-function>',
    '<transform-list>',
    '<url>'
  ]
}

function with_style_node(text, fn) {
  let node = document.createElement('style');
  node.textContent = text;
  try {
    document.body.append(node);
    fn(node);
  } finally {
    node.remove();
  }
}

function with_at_property(desc, fn) {
  let name = typeof(desc.name) === 'undefined' ? generate_name() : desc.name;
  let text = `@property ${name} {`;
  if (typeof(desc.syntax) !== 'undefined')
    text += `syntax:${desc.syntax};`;
  if (typeof(desc.initialValue) !== 'undefined')
    text += `initial-value:${desc.initialValue};`;
  if (typeof(desc.inherits) !== 'undefined')
    text += `inherits:${desc.inherits};`;
  text += '}';
  with_style_node(text, (node) => fn(name, node.sheet.rules[0]));
}

function test_with_at_property(desc, fn, description) {
  test(() => with_at_property(desc, fn), description);
}

function test_with_style_node(text, fn, description) {
  test(() => with_style_node(text, fn), description);
}

function animation_test(property, values, description) {
  const name = generate_name();
  property.name = name;
  CSS.registerProperty(property);

  test(() => {
    const duration = 1000;
    const keyframes = {};
    keyframes[name] = values.keyframes;

    const iterations = 3;
    const composite = values.composite || "replace";
    const iterationComposite = values.iterationComposite || "replace";
    const animation = target.animate(keyframes, { composite, iterationComposite, iterations, duration });
    animation.pause();
    // We seek to the middle of the third iteration which will allow to test cases where
    // iterationComposite is set to something other than "replace".
    animation.currentTime = duration * 2.5;

    const assert_equals_function = values.assert_function || assert_equals;
    assert_equals_function(getComputedStyle(target).getPropertyValue(name), values.expected);
  }, description);
};

function discrete_animation_test(syntax, fromValue, toValue, description) {
  test(() => {
    const name = generate_name();

    CSS.registerProperty({
      name,
      syntax,
      inherits: false,
      initialValue: fromValue
    });

    const duration = 1000;
    const keyframes = [];
    keyframes[name] = toValue;
    const animation = target.animate(keyframes, duration);
    animation.pause();

    const checkAtProgress = (progress, expected) => {
      animation.currentTime = duration * 0.25;
      assert_equals(getComputedStyle(target).getPropertyValue(name), fromValue, `The correct value is used at progress = ${progress}`);
    };

    checkAtProgress(0, fromValue);
    checkAtProgress(0.25, fromValue);
    checkAtProgress(0.49, fromValue);
    checkAtProgress(0.5, toValue);
    checkAtProgress(0.75, toValue);
    checkAtProgress(1, toValue);
  }, description || `Animating a custom property of type ${syntax} is discrete`);
}

function transition_test(options, description) {
  promise_test(async () => {
    const customProperty = generate_name();

    options.transitionProperty ??= customProperty;

    CSS.registerProperty({
      name: customProperty,
      syntax: options.syntax,
      inherits: false,
      initialValue: options.from
    });

    assert_equals(getComputedStyle(target).getPropertyValue(customProperty), options.from, "Element has the expected initial value");

    const transitionEventPromise = new Promise(resolve => {
      let listener = event => {
          target.removeEventListener("transitionrun", listener);
          assert_equals(event.propertyName, customProperty, "TransitionEvent has the expected property name");
          resolve();
      };
      target.addEventListener("transitionrun", listener);
    });

    target.style.transition = `${options.transitionProperty} 1s -500ms linear`;
    if (options.behavior) {
      target.style.transitionBehavior = options.behavior;
    }
    target.style.setProperty(customProperty, options.to);

    const animations = target.getAnimations();
    assert_equals(animations.length, 1, "A single animation is running");

    const transition = animations[0];
    assert_class_string(transition, "CSSTransition", "A CSSTransition is running");

    transition.pause();
    assert_equals(getComputedStyle(target).getPropertyValue(customProperty), options.expected, "Element has the expected animated value");

    await transitionEventPromise;
  }, description);
}

function no_transition_test(options, description) {
  test(() => {
    const customProperty = generate_name();

    CSS.registerProperty({
      name: customProperty,
      syntax: options.syntax,
      inherits: false,
      initialValue: options.from
    });

    assert_equals(getComputedStyle(target).getPropertyValue(customProperty), options.from, "Element has the expected initial value");

    target.style.transition = `${customProperty} 1s -500ms linear`;
    target.style.setProperty(customProperty, options.to);

    assert_equals(target.getAnimations().length, 0, "No animation was created");
    assert_equals(getComputedStyle(target).getPropertyValue(customProperty), options.to, "Element has the expected final value");
  }, description);
};

function test_initial_value_valid(syntax, initialValue) {
    // No actual assertions, this just shouldn't throw
    test(() => {
        var name = generate_name();
        CSS.registerProperty({name: name, syntax: syntax, initialValue: initialValue, inherits: false});
    }, "syntax:'" + syntax + "', initialValue:'" + initialValue + "' is valid");
}

function test_initial_value_invalid(syntax, initialValue) {
    test(() =>{
        var name = generate_name();
        assert_throws_dom("SyntaxError",
            () => CSS.registerProperty({name: name, syntax: syntax, initialValue: initialValue, inherits: false}));
    }, "syntax:'" + syntax + "', initialValue:'" + initialValue + "' is invalid");
}