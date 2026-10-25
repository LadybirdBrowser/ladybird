function TestCase(scenarios, sanityChecker) {
  function runTest(scenario) {
    sanityChecker.checkScenario(scenario, subresourceMap);

    const urls = getRequestURLs(scenario.subresource,
                                scenario.origin,
                                scenario.redirection);

    /** @type {Subresource} */
    const subresource = {
      subresourceType: scenario.subresource,
      url: urls.testUrl,
      policyDeliveries: scenario.subresource_policy_deliveries,
    };

    let violationEventResolve;
    // Resolved with an array of securitypolicyviolation events.
    const violationEventPromise = new Promise(resolve => {
        violationEventResolve = resolve;
      });

    promise_test(async t => {
      await xhrRequest(urls.announceUrl);

      // Currently only requests from top-level Documents are tested
      // (specified by `spec.src.json`) and thus securitypolicyviolation
      // events are assumed to be fired on the top-level Document here.
      // When adding non-top-level Document tests, securitypolicyviolation
      // events should be caught in appropriate contexts.
      const violationEvents = [];
      const listener = e => { violationEvents.push(e); };
      document.addEventListener('securitypolicyviolation', listener);

      try {
        // Send out the real resource request.
        // This should tear down the key if it's not blocked.
        const mainPromise = invokeRequest(subresource, scenario.source_context_list);
        if (scenario.expectation === 'allowed') {
          await mainPromise;
        } else {
          await mainPromise
              .then(t.unreached_func('main promise resolved unexpectedly'))
              .catch(_ => {});
        }
      } finally {
        // Always perform post-processing/clean up for
        // 'securitypolicyviolation' events and resolve
        // `violationEventPromise`, to prevent timeout of the
        // promise_test() below.

        // securitypolicyviolation events are fired in a queued task in
        // https://w3c.github.io/webappsec-csp/#report-violation
        // so wait for queued tasks to run using setTimeout().
        let timeout = 0;
        if (scenario.subresource.startsWith('worklet-') &&
            navigator.userAgent.includes("Firefox/")) {
          // https://bugzilla.mozilla.org/show_bug.cgi?id=1808911
          // In Firefox sometimes violations from Worklets are delayed.
          timeout = 10;
        } else if (scenario.subresource.startsWith('worker-') &&
                   navigator.userAgent.includes("Servo/")) {
          // In Servo, worker violations are also delayed, as they are
          // sent via IPC. However, they typically arrive relatively
          // quickly after that.
          timeout = 1;
        }
        await new Promise(resolve => setTimeout(resolve, timeout));

        // Pass violation events to `violationEventPromise` (which will be tested
        // in the subsequent promise_test()) and clean up the listener.
        violationEventResolve(violationEvents);
        document.removeEventListener('securitypolicyviolation', listener);
      }

      // Send request to check if the key has been torn down.
      const assertResult = await xhrRequest(urls.assertUrl);

      // Now check if the value has been torn down. If it's still there,
      // we have blocked the request by content security policy.
      assert_equals(assertResult.status, scenario.expectation,
        "The resource request should be '" + scenario.expectation + "'.");

    }, scenario.test_description);

    promise_test(async _ => {
        const violationEvents = await violationEventPromise;
        if (scenario.expectation === 'allowed') {
          assert_array_equals(violationEvents, [],
              'no violation events should be fired');
        } else {
          assert_equals(violationEvents.length, 1,
              'One violation event should be fired');
        }
      }, scenario.test_description + ": securitypolicyviolation");
  }  // runTest

  function runTests() {
    for (const scenario of scenarios) {
      runTest(scenario);
    }
  }

  return {start: runTests};
}
