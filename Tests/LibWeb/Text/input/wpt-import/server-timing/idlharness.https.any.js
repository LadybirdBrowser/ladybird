// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: timeout=long

'use strict';

// https://w3c.github.io/server-timing/

idl_test(
  ['resource-timing', 'server-timing'],
  ['performance-timeline', 'hr-time', 'dom'],
  idl_array => new Promise((resolve, reject) => {
    new PerformanceObserver(entries => {
      entries.getEntries().forEach(e => {
        if (!e.name.endsWith('blue.png'))
          return;

        self.resource = e;
        self.server = e.serverTiming[0];
        idl_array.add_objects({
          PerformanceResourceTiming: ['resource'],
          PerformanceServerTiming: ['server']
        });
        resolve();
      })
    }).observe({entryTypes: ['resource']});
    fetch('resources/blue.png');
  })
);
