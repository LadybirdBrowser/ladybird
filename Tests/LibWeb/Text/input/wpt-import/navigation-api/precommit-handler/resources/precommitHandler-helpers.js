window.testDeferredCommit = async (t, navigationType, mode, destinationIndex = 0) => {
  let startHash = location.hash;
  let destinationHash;
  const err = new Error("boo!");

  let popstate_fired = false;
  window.addEventListener("popstate", () => popstate_fired = true, { once : true });
  let navigatesuccess_fired = false;
  navigation.addEventListener("navigatesuccess", () => navigatesuccess_fired = true, { once : true });
  let navigateerror_fired = false;
  navigation.addEventListener("navigateerror", () => navigateerror_fired = true, { once : true });

  // mode-specific logic for the navigate event handler
  let navigate_helpers = {
    rejectBeforeCommit : async (e) => { return Promise.reject("Should never run") },
    rejectAfterCommit : async (e) => {
      assert_equals(location.hash, destinationHash, "hash after commit");
      assert_equals(false, popstate_fired, "popstate before handler starts");
      await new Promise(resolve => t.step_timeout(resolve, 0));
      assert_equals(navigationType == "traverse", popstate_fired, "popstate fired after handler async step");
      return Promise.reject(err);
    },
    success : async (e) => {
      assert_equals(location.hash, destinationHash, "hash after commit");
      assert_equals(false, popstate_fired, "popstate before handler starts");
      await new Promise(resolve => t.step_timeout(resolve, 0));
      assert_equals(navigationType == "traverse", popstate_fired, "popstate fired after handler async step");
      return new Promise(resolve => t.step_timeout(resolve, 0));
    },
  }

  navigation.addEventListener("navigate", e => {
    e.intercept({ precommitHandler: t.step_func(async () => {
                    assert_equals(e.navigationType, navigationType);
                    assert_equals(location.hash, startHash, "start hash");
                    assert_false(popstate_fired, "popstate fired at handler start");

                    await new Promise(resolve => t.step_timeout(resolve, 0));
                    assert_equals(location.hash, startHash, "hash after first async step");
                    assert_false(popstate_fired, "popstate fired after first async step");

                    if (mode == "rejectBeforeCommit")
                      return Promise.reject(err);
                  }),
                  handler: t.step_func(navigate_helpers[mode])
                });
  }, { once: true });

  let startingIndex = navigation.currentEntry.index;
  let expectedIndexOnCommit;

  let promises;
  if (navigationType === "push" || navigationType === "replace") {
    destinationHash = (startHash === "" ? "#" : startHash) + "a";
    promises = navigation.navigate(destinationHash, { history: navigationType });
    expectedIndexOnCommit = (navigationType === "push") ? startingIndex + 1
                                                        : startingIndex;
  } else if (navigationType === "reload") {
    destinationHash = startHash;
    promises = navigation.reload();
    expectedIndexOnCommit = startingIndex;
  } else if (navigationType === "traverse") {
    let destinationEntry = navigation.entries()[destinationIndex];
    destinationHash = new URL(destinationEntry.url).hash;
    promises = navigation.traverseTo(destinationEntry.key);
    expectedIndexOnCommit = destinationIndex;
  }

  if (mode === "rejectBeforeCommit") {
    await assertBothRejectExactly(t, promises, err);
    assert_equals(location.hash, startHash, "hash after promise resolution");
    assert_false(popstate_fired, "popstate fired after promise resolution");
    assert_false(navigatesuccess_fired, "navigatesuccess fired");
    assert_true(navigateerror_fired, "navigateerror fired");
    assert_equals(navigation.currentEntry.index, startingIndex);
  } else if (mode === "rejectAfterCommit") {
    await promises.committed;
    await assertCommittedFulfillsFinishedRejectsExactly(t, promises, navigation.currentEntry, err);
    assert_equals(location.hash, destinationHash, "hash after promise resolution");
    assert_equals(navigationType == "traverse", popstate_fired, "popstate fired after promise resolution");
    assert_false(navigatesuccess_fired, "navigatesuccess fired");
    assert_true(navigateerror_fired, "navigateerror fired");
    assert_equals(navigation.currentEntry.index, expectedIndexOnCommit);
  } else {
    await promises.committed;
    await assertBothFulfill(t, promises, navigation.currentEntry);
    assert_equals(location.hash, destinationHash, "hash after promise resolution");
    assert_equals(navigationType == "traverse", popstate_fired, "popstate fired after promise resolution");
    assert_true(navigatesuccess_fired, "navigatesuccess fired");
    assert_false(navigateerror_fired, "navigateerror fired");
    assert_equals(navigation.currentEntry.index, expectedIndexOnCommit);
  }
}
