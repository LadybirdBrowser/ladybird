// Compares a mutation record to a predefined one
// mutationToCheck is a mutation record from the user agent
// expectedRecord is a mutation record minted by the test
//    for expectedRecord, if properties are omitted, they get default ones
function checkRecords(target, mutationToCheck, expectedRecord) {
  var mr1;
  var mr2;


  function checkField(property, isArray) {
    var field = mr2[property];
    if (isArray === undefined) {
      isArray = false;
    }
    if (field instanceof Function) {
      field = field();
    } else if (field === undefined) {
      if (isArray) {
        field = new Array();
      } else {
        field = null;
      }
    }
    if (isArray) {
      assert_array_equals(mr1[property], field, property + " didn't match");
    } else {
      assert_equals(mr1[property], field, property + " didn't match");
    }
  }

  assert_equals(mutationToCheck.length, expectedRecord.length, "mutation records must match");
  for (var item = 0; item < mutationToCheck.length; item++) {
    mr1 = mutationToCheck[item];
    mr2 = expectedRecord[item];

    if (mr2.target instanceof Function) {
      assert_equals(mr1.target, mr2.target(), "target node must match");
    } else if (mr2.target !== undefined) {
      assert_equals(mr1.target, mr2.target, "target node must match");
    } else {
      assert_equals(mr1.target, target, "target node must match");
    }

    checkField("type");
    checkField("addedNodes", true);
    checkField("removedNodes", true);
    checkField("previousSibling");
    checkField("nextSibling");
    checkField("attributeName");
    checkField("attributeNamespace");
    checkField("oldValue");
  };
}

function runMutationTest(node, mutationObserverOptions, mutationRecordSequence, mutationFunction, description, target) {
  var test = async_test(description);


  function moc(mrl, obs) {
    test.step(
      function () {
            if (target === undefined) target = node;
            checkRecords(target, mrl, mutationRecordSequence);
        test.done();
      }
     );
  }

  test.step(
    function () {
      (new MutationObserver(moc)).observe(node, mutationObserverOptions);
      mutationFunction();
    }
  );
  return mutationRecordSequence.length
}
