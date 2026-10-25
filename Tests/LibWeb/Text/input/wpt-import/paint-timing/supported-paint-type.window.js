test(() => {
  assert_implements(typeof PerformanceObserver.supportedEntryTypes !== "undefined", 'supportedEntryTypes is not supported');
  assert_true(PerformanceObserver.supportedEntryTypes.includes("paint"),
    "There should be an entry 'paint' in PerformanceObserver.supportedEntryTypes");
}, "supportedEntryTypes contains 'paint'.");

const entryType = 'paint';
promise_test(async() => {
  assert_implements(typeof PerformanceObserver.supportedEntryTypes !== "undefined", 'supportedEntryTypes is not supported');
  assert_implements(typeof PerformanceObserver.supportedEntryTypes.includes(entryType), `supportedEntryTypes does not include '${entryType}'`);
  await new Promise((resolve) => {
    new PerformanceObserver(function (list, observer) {
      observer.disconnect();
      resolve();
    }).observe({entryTypes: [entryType]});

    // Force the PerformanceEntry.
    // Use `self` for Workers.
    if (self.document)
      document.head.parentNode.appendChild(document.createTextNode('foo'));
  })
}, `'${entryType}' entries should be observable.`)
