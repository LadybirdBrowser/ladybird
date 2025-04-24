const Behavior = Object.freeze({
  ReflectsHost: 'ReflectsHost',
  ReflectsHostInArray: 'ReflectsHostInArray',
  IsNull: 'IsNull',
  ReflectsHostID: 'ReflectsHostID',
  ReflectsHostIDInDOMTokenList: 'ReflectsHostIDInDOMTokenList',
});

// We want to test types of elements that are associated with properties that can reflect other
// elements and can therefore interact with reference target in interesting ways.
// The HTML5_LABELABLE_ELEMENTS are defined in https://html.spec.whatwg.org/#category-label,
// while non_labelable_element_types is a manually curated list of other elements with
// reflecting properties (plus div as representative of more "normal" elements).
// We'll test all permutations of these element types being both the referencing element
// pointing into the reference target shadow host, and being the referenced element inside
// the shadow.
const non_labelable_element_types = ["div", "object", "label", "fieldset", "legend", "option", "datalist", "form"];
const element_types = HTML5_LABELABLE_ELEMENTS.concat(non_labelable_element_types);

function test_property_reflection(element_creation_method, test_name_suffix, referencing_element_type, referenced_element_type, attribute, reflected_property, expected_behavior) {
  // There's nothing to test if the referencing element type doesn't have the reflecting
  // property.
  if (!(reflected_property in document.createElement(referencing_element_type))) {
    return;
  }

  test(function () {
    const referencing_element = document.createElement(referencing_element_type);
    document.body.appendChild(referencing_element);
    referencing_element.setAttribute(attribute, "host-id");
    const host_container = document.querySelector("#host-container");
    const host = element_creation_method(host_container, referenced_element_type);
    if (expected_behavior === Behavior.ReflectsHost) {
      assert_equals(referencing_element[reflected_property], host);
    } else if (expected_behavior === Behavior.ReflectsHostInArray) {
      assert_array_equals(referencing_element[reflected_property], [host]);
    } else if (expected_behavior === Behavior.IsNull) {
      assert_equals(referencing_element[reflected_property], null);
    } else if (expected_behavior === Behavior.ReflectsHostID) {
      assert_equals(referencing_element[reflected_property], "host-id");
    } else if (expected_behavior === Behavior.ReflectsHostIDInDOMTokenList) {
      assert_true(referencing_element[reflected_property] instanceof DOMTokenList);
      assert_array_equals(Array.from(referencing_element[reflected_property]), ["host-id"]);
    }
    referencing_element.remove();
    host_container.setHTMLUnsafe("");
  }, `${referencing_element_type}.${reflected_property} has reflection behavior ${expected_behavior} when pointing to ${referenced_element_type} with reference target${test_name_suffix}`);
}

function run_test_for_all_reflecting_properties(setup_function, test_function, test_name_suffix) {
  for(let referencing_element_type of element_types) {
    for(let referenced_element_type of element_types) {
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-controls", "ariaControlsElements", Behavior.ReflectsHostInArray);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-activedescendant", "ariaActiveDescendantElement", Behavior.ReflectsHost);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-describedby", "ariaDescribedByElements", Behavior.ReflectsHostInArray);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-details", "ariaDetailsElements", Behavior.ReflectsHostInArray);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-errormessage", "ariaErrorMessageElements", Behavior.ReflectsHostInArray);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-flowto", "ariaFlowToElements", Behavior.ReflectsHostInArray);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-labelledby", "ariaLabelledByElements", Behavior.ReflectsHostInArray);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "aria-owns", "ariaOwnsElements", Behavior.ReflectsHostInArray);

      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "anchor", "anchorElement", Behavior.ReflectsHost);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "commandfor", "commandForElement", Behavior.ReflectsHost);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "popovertarget", "popoverTargetElement", Behavior.ReflectsHost);
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "interesttarget", "interestTargetElement", Behavior.ReflectsHost);

      const expected_htmlFor_property_behavior = (referencing_element_type == "output") ? Behavior.ReflectsHostIDInDOMTokenList : Behavior.ReflectsHostID;
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "for", "htmlFor", expected_htmlFor_property_behavior);

      // The form property of <label>, <legend>, and <option> reflects the form property of the associated labelable element,
      // the associated <fieldset>, and the associated <select>, respectively. Here since we don't have those associated elements,
      // the form property would return null.
      const expected_form_property_behavior = (referenced_element_type == 'form' &&
                                              referencing_element_type != "label" &&
                                              referencing_element_type != "legend" &&
                                              referencing_element_type != "option") ? Behavior.ReflectsHost : Behavior.IsNull;
                                              test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "form", "form", expected_form_property_behavior);

      const expected_list_property_behavior = (referenced_element_type == 'datalist') ? Behavior.ReflectsHost : Behavior.IsNull;
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "list", "list", expected_list_property_behavior);

      const expected_control_property_behavior = HTML5_LABELABLE_ELEMENTS.includes(referenced_element_type) ? Behavior.ReflectsHost : Behavior.IsNull;
      test_function(setup_function, test_name_suffix, referencing_element_type, referenced_element_type, "for", "control", expected_control_property_behavior);
    }
  }
}