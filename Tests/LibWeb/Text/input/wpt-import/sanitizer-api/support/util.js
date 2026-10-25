const XHTML_NS = "http://www.w3.org/1999/xhtml";

// https://wicg.github.io/sanitizer-api/#built-in-non-replaceable-elements-list
const NON_REPLACEABLE_ELEMENTS = [
  "html", // Cover both spellings
  { name: "html", namespace: XHTML_NS },
  { name: "svg", namespace: "http://www.w3.org/2000/svg" },
  { name: "math", namespace: "http://www.w3.org/1998/Math/MathML" },
];

function is_same_sanitizer_name(a, b) {
  return a.name === b.name && a.namespace === b.namespace;
}

function is_same_sanitizer_target(a, b) {
  return a.target === b.target;
}

function is_data_attribute(attribute) {
  return attribute.name.startsWith("data-") && attribute.namespace === null;
}

function has_duplicates(list) {
  if (!list) {
    return false;
  }

  for (let i = 0; i < list.length; i++) {
    for (let j = i + 1; j < list.length; j++) {
      if (is_same_sanitizer_name(list[i], list[j])) {
        return true;
      }
    }
  }

  return false;
}

function has_duplicate_targets(list) {
  if (!list) {
    return false;
  }

  for (let i = 0; i < list.length; i++) {
    for (let j = i + 1; j < list.length; j++) {
      if (is_same_sanitizer_target(list[i], list[j])) {
        return true;
      }
    }
  }

  return false;
}

function has_intersection(list1, list2) {
  if (!list1 || !list2) {
    return false;
  }

  for (let item1 of list1) {
    for (let item2 of list2) {
      if (is_same_sanitizer_name(item1, item2)) {
        return true;
      }
    }
  }

  return false;
}

function is_subset(subset, superset) {
  if (!subset) {
    return true;
  }

  for (let item of subset) {
    if (!superset.some(superItem => is_same_sanitizer_name(item, superItem))) {
      return false;
    }
  }

  return true;
}

// https://wicg.github.io/sanitizer-api/#sanitizerconfig-valid
function assert_config_is_valid(config) {
  // 1. Assert: config["elements"] exists or config["removeElements"] exists.
  assert_true(
    "elements" in config || "removeElements" in config,
    'Assert: config["elements"] exists or config["removeElements"] exists.',
  );

  // 2. If config["elements"] exists and config["removeElements"] exists, then return false.
  assert_false(
    "elements" in config && "removeElements" in config,
    'If config["elements"] exists and config["removeElements"] exists, then return false.',
  );

  // 3. Assert: Either config["processingInstructions"] exists or config["removeProcessingInstructions"] exists.
  assert_true(
    "processingInstructions" in config || "removeProcessingInstructions" in config,
    'Assert: Either config["processingInstructions"] exists or config["removeProcessingInstructions"] exists.',
  );

  // 4. If config["processingInstructions"] exists and config["removeProcessingInstructions"] exists, then return false.
  assert_false(
    "processingInstructions" in config && "removeProcessingInstructions" in config,
    'If config["processingInstructions"] exists and config["removeProcessingInstructions"] exists, then return false.',
  );

  // 5. Assert: Either config["attributes"] exists or config["removeAttributes"] exists.
  assert_true(
    "attributes" in config || "removeAttributes" in config,
    'Assert: Either config["attributes"] exists or config["removeAttributes"] exists.',
  );

  // 6. If config["attributes"] exists and config["removeAttributes"] exists, then return false.
  assert_false(
    "attributes" in config && "removeAttributes" in config,
    'If config["attributes"] exists and config["removeAttributes"] exists, then return false.',
  );

  // 7. Assert: All SanitizerElementNamespaceWithAttributes, SanitizerElementNamespace, SanitizerProcessingInstruction, and SanitizerAttributeNamespace items in config are canonical, meaning they have been run through canonicalize a sanitizer element, canonicalize a sanitizer processing instruction, or canonicalize a sanitizer attribute, as appropriate.

  // 8. If config["elements"] exists:
  if ("elements" in config) {
    // 8.1. If config["elements"] has duplicates, then return false.
    assert_false(has_duplicates(config.elements), 'If config["elements"] has duplicates, then return false.');
  } else {
    // 9. Otherwise:
    // 9.1. If config["removeElements"] has duplicates, then return false.
    assert_false(has_duplicates(config.removeElements), 'If config["removeElements"] has duplicates, then return false.');
  }

  // 10. If config["replaceWithChildrenElements"] exists and has duplicates, then return false.
  if (config.replaceWithChildrenElements) {
    assert_false(has_duplicates(config.replaceWithChildrenElements), 'If config["replaceWithChildrenElements"] exists and has duplicates, then return false.');
  }

  // 11. If config["processingInstructions"] exists:
  if ("processingInstructions" in config) {
    // 11.1. If config["processingInstructions"] has duplicate targets, then return false.
    assert_false(has_duplicate_targets(config.processingInstructions), 'If config["processingInstructions"] has duplicate targets, then return false.')
  } else {
    // 12. Otherwise:
    // 12.1. If config["removeProcessingInstructions"] has duplicate targets, then return false.
    assert_false(has_duplicate_targets(config.removeProcessingInstructions), 'If config["removeProcessingInstructions"] has duplicate targets, then return false.')
  }

  // 13. If config["attributes"] exists:
  if ("attributes" in config) {
    // 13.1. If config["attributes"] has duplicates, then return false.
    assert_false(has_duplicates(config.attributes), 'If config["attributes"] has duplicates, then return false.');
  } else {
    // 14. Otherwise:
    // 14.1. If config["removeAttributes"] has duplicates, then return false.
    assert_false(has_duplicates(config.removeAttributes), 'If config["removeAttributes"] has duplicates, then return false.');
  }

  // 15. If config["replaceWithChildrenElements"] exists:
  if (config.replaceWithChildrenElements) {
    // 15.1. If config["elements"] exists:
    if (config.elements) {
      // 15.1.1. If the intersection of config["elements"] and config["replaceWithChildrenElements"] is not empty, then return false.
      assert_false(
        has_intersection(config.elements, config.replaceWithChildrenElements),
        'If the intersection of config["elements"] and config["replaceWithChildrenElements"] is not empty, then return false.',
      );
    } else {
      // 15.2. Otherwise:
      // 15.2.1. If the intersection of config["removeElements"] and config["replaceWithChildrenElements"] is not empty, then return false.
      assert_false(
        has_intersection(config.removeElements, config.replaceWithChildrenElements),
        'If the intersection of config["removeElements"] and config["replaceWithChildrenElements"] is not empty, then return false.',
      );
    }
  }

  // 16. If config["attributes"] exists:
  if (config.attributes) {
    // 16.1. Assert: config["dataAttributes"] exists.
    assert_true("dataAttributes" in config, 'Assert: config["dataAttributes"] exists.');

    // 16.2. If config["elements"] exists:
    if (config.elements) {
      // 16.2.1. For each element of config["elements"]:
      for (let element of config.elements) {
        // 16.2.1.1. If element["attributes"] exists and element["attributes"] has duplicates, then return false.
        if (element.attributes) {
          assert_false(has_duplicates(element.attributes),
                      `If element["attributes"] exists and element["attributes"] has duplicates, then return false. (element: ${element.name})`);
        }

        // 16.2.1.2. If element["removeAttributes"] exists and element["removeAttributes"] has duplicates, then return false.
        if (element.removeAttributes) {
          assert_false(has_duplicates(element.removeAttributes),
                      `If element["removeAttributes"] exists and element["removeAttributes"] has duplicates, then return false. (element: ${element.name})`);
        }

        // 16.2.1.3. If the intersection of config["attributes"] and element["attributes"] with default « » is not empty, then return false.
        assert_false(has_intersection(config.attributes, element.attributes),
                    `If the intersection of config["attributes"] and element["attributes"] with default « » is not empty, then return false. (element: ${element.name})`);

        // 16.2.1.4. If element["removeAttributes"] with default « » is not a subset of config["attributes"], then return false.
        assert_true(is_subset(element.removeAttributes, config.attributes),
                    `If element["removeAttributes"] with default « » is not a subset of config["attributes"], then return false. (element: ${element.name})`);

        // 16.2.1.5. If config["dataAttributes"] is true and element["attributes"] contains a custom data attribute, then return false.
        if (config.dataAttributes && element.attributes) {
          for (let attr of element.attributes) {
            assert_false(is_data_attribute(attr),
                        `If config["dataAttributes"] is true and element["attributes"] contains a custom data attribute, then return false. (element: ${element.name}, attribute: ${attr.name})`);
          }
        }
      }
    }

    // 16.3. If config["dataAttributes"] is true and config["attributes"] contains a custom data attribute, then return false.
    if (config.dataAttributes) {
      for (let attr of config.attributes) {
        assert_false(is_data_attribute(attr),
                    `If config["dataAttributes"] is true and config["attributes"] contains a custom data attribute, then return false. (attribute: ${attr.name})`);
      }
    }
  } else {
    // 17. Otherwise:
    // 17.1. If config["elements"] exists:
    if (config.elements) {
      // 17.1.1. For each element of config["elements"]:
      for (let element of config.elements) {
        // 17.1.1.1. If element["attributes"] exists and element["removeAttributes"] exists, then return false.
        assert_false("attributes" in element && "removeAttributes" in element,
                     `If element["attributes"] exists and element["removeAttributes"] exists, then return false. (element: ${element.name})`);

        // 17.1.1.2. If element["attributes"] exist and element["attributes"] has duplicates, then return false.
        if (element.attributes) {
          assert_false(has_duplicates(element.attributes),
                      `If element["attributes"] exist and element["attributes"] has duplicates, then return false. (element: ${element.name})`);
        }

        // 17.1.1.3. If element["removeAttributes"] exist and element["removeAttributes"] has duplicates, then return false.
        if (element.removeAttributes) {
          assert_false(has_duplicates(element.removeAttributes),
                      `If element["removeAttributes"] exist and element["removeAttributes"] has duplicates, then return false. (element: ${element.name})`);
        }

        // 17.1.1.4. If the intersection of config["removeAttributes"] and element["attributes"] with default « » is not empty, then return false.
        if (element.attributes) {
          assert_false(has_intersection(config.removeAttributes, element.attributes),
                      `If the intersection of config["removeAttributes"] and element["attributes"] with default « » is not empty, then return false. (element: ${element.name})`);
        }

        // 17.1.1.5. If the intersection of config["removeAttributes"] and element["removeAttributes"] with default « » is not empty, then return false.
        if (element.removeAttributes) {
          assert_false(has_intersection(config.removeAttributes, element.removeAttributes),
                      `If the intersection of config["removeAttributes"] and element["removeAttributes"] with default « » is not empty, then return false. (element: ${element.name})`);
        }
      }
    }

    // 17.2. If config["dataAttributes"] exists, then return false.
    assert_false("dataAttributes" in config, 'If config["dataAttributes"] exists, then return false.');
  }

  // 18. Return true.
}

function assert_config(config, expected) {
  const PROPERTIES = [
    "attributes",
    "removeAttributes",
    "processingInstructions",
    "removeProcessingInstructions",
    "elements",
    "removeElements",
    "replaceWithChildrenElements",
    "comments",
    "dataAttributes",
    "javascriptURLs",
  ];

  // Prevent some typos in the expected config.
  for (let key of Object.keys(expected)) {
    assert_in_array(key, PROPERTIES, "expected");
  }
  for (let key of Object.keys(config)) {
    assert_in_array(key, PROPERTIES, "config");
  }

  assert_config_is_valid(config);

  function assert_attrs(key, config, expected, prefix = "config") {
    // XXX we allow specifying only a subset for expected.
    if (!(key in expected)) {
      return;
    }

    if (expected[key] === undefined) {
      assert_false(key in config, `Unexpected '${key}' in ${prefix}`);
      return;
    }

    assert_true(key in config, `Missing '${key}' from ${prefix}`);
    assert_equals(config[key]?.length, expected[key].length, `${prefix}.${key}.length`);
    for (let i = 0; i < expected[key].length; i++) {
      let attribute = expected[key][i];
      if (typeof attribute === "string") {
        assert_object_equals(
          config[key][i],
          { name: attribute, namespace: null },
          `${prefix}.${key}[${i}] should match`,
        );
      } else {
        assert_object_equals(
          config[key][i],
          attribute,
          `${prefix}.${key}[${i}] should match`,
        );
      }
    }
  }

  assert_attrs("attributes", config, expected);
  assert_attrs("removeAttributes", config, expected);

  function assert_pis(key, config, expected, prefix = "config") {
    if (!(key in expected)) {
      return;
    }

    if (expected[key] === undefined) {
      assert_false(key in config, `Unexpected '${key}' in ${prefix}`);
      return;
    }

    assert_true(key in config, `Missing '${key}' from ${prefix}`);
    assert_equals(config[key]?.length, expected[key].length, `${prefix}.${key}.length`);
    for (let i = 0; i < expected[key].length; i++) {
      let processingInstructions = expected[key][i];
      if (typeof processingInstructions === "string") {
        assert_object_equals(
          config[key][i],
          { target: processingInstructions },
          `${prefix}.${key}[${i}] should match`,
        );
      } else {
        assert_object_equals(
          config[key][i],
          processingInstructions,
          `${prefix}.${key}[${i}] should match`,
        );
      }
    }
  }

  assert_pis("processingInstructions", config, expected);
  assert_pis("removeProcessingInstructions", config, expected);

  function assert_elems(key) {
    if (!(key in expected)) {
      return;
    }

    if (expected[key] === undefined) {
      assert_false(key in config, `Unexpected '${key}' in config`);
      return;
    }

    assert_true(key in config, `Missing '${key}' from config`);
    assert_equals(config[key]?.length, expected[key].length, `${key}.length`);

    for (let i = 0; i < expected[key].length; i++) {
      let element = expected[key][i];
      // To make writing tests a bit easier we also support the shorthand string syntax.
      if (typeof element === "string") {
        let extra = key === "elements" ? { removeAttributes: [] } : { };
        assert_object_equals(
          config[key][i],
          { name: element, namespace: XHTML_NS, ...extra },
          `${key}[${i}] should match`,
        );
      } else {
        if (key === "elements") {
          assert_equals(config[key][i].name, element.name, `${key}[${i}].name should match`);
          let ns = "namespace" in element ? element.namespace : XHTML_NS;
          assert_equals(config[key][i].namespace, ns, `${key}[${i}].namespace should match`);

          assert_attrs("attributes", config[key][i], element, `config.elements[${i}]`);
          assert_attrs("removeAttributes", config[key][i], element, `config.elements[${i}]`);
        } else {
          assert_object_equals(config[key][i], element, `${key}[${i}] should match`);
        }
      }
    }
  }

  assert_elems("elements");
  assert_elems("removeElements");
  assert_elems("replaceWithChildrenElements");

  if ("comments" in expected) {
    assert_equals(config.comments, expected.comments, "comments should match");
  }

  if ("dataAttributes" in expected) {
    assert_equals(config.dataAttributes, expected.dataAttributes, "dataAttributes should match");
  }

  if ("javascriptURLs" in expected) {
    assert_equals(config.javascriptURLs, expected.javascriptURLs, "javascriptURLs should match");
  }
}
