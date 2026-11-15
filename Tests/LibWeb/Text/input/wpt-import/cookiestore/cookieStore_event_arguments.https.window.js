'use strict';

test(() => {
  const event = new CookieChangeEvent('change');
  assert_true(event instanceof CookieChangeEvent);
  assert_equals(event.type, 'change');
  assert_equals(event.changed.length, 0);
  assert_equals(event.deleted.length, 0);
}, 'CookieChangeEvent construction with default arguments');

test(() => {
  const event = new CookieChangeEvent('change', {
    changed: [
      { name: 'changed-name1', value: 'changed-value1' },
      { name: 'changed-name2', value: 'changed-value2' },
    ],
  });
  assert_true(event instanceof CookieChangeEvent);
  assert_equals(event.type, 'change');
  assert_equals(event.changed.length, 2);
  assert_equals(event.changed[0].name, 'changed-name1');
  assert_equals(event.changed[0].value, 'changed-value1');
  assert_equals(event.changed[1].name, 'changed-name2');
  assert_equals(event.changed[1].value, 'changed-value2');
  assert_equals(event.deleted.length, 0);
}, 'CookieChangeEvent construction with changed cookie list');

test(() => {
  const event = new CookieChangeEvent('change', {
    deleted: [
      { name: 'deleted-name1', value: 'deleted-value1' },
      { name: 'deleted-name2', value: 'deleted-value2' },
    ],
  });
  assert_true(event instanceof CookieChangeEvent);
  assert_equals(event.type, 'change');
  assert_equals(event.changed.length, 0);
  assert_equals(event.deleted.length, 2);
  assert_equals(event.deleted[0].name, 'deleted-name1');
  assert_equals(event.deleted[0].value, 'deleted-value1');
  assert_equals(event.deleted[1].name, 'deleted-name2');
  assert_equals(event.deleted[1].value, 'deleted-value2');
}, 'CookieChangeEvent construction with deleted cookie list');

test(() => {
  const event = new CookieChangeEvent('change', {
    changed: [
      { name: 'changed-name1', value: 'changed-value1' },
      { name: 'changed-name2', value: 'changed-value2' },
    ],
    deleted: [
      { name: 'deleted-name1', value: 'deleted-value1' },
    ],
  });
  assert_true(event instanceof CookieChangeEvent);
  assert_equals(event.type, 'change');
  assert_equals(event.changed.length, 2);
  assert_equals(event.changed[0].name, 'changed-name1');
  assert_equals(event.changed[0].value, 'changed-value1');
  assert_equals(event.changed[1].name, 'changed-name2');
  assert_equals(event.changed[1].value, 'changed-value2');
  assert_equals(event.deleted.length, 1);
  assert_equals(event.deleted[0].name, 'deleted-name1');
  assert_equals(event.deleted[0].value, 'deleted-value1');
}, 'CookieChangeEvent construction with changed and deleted cookie lists');