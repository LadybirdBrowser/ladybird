'use strict';

test(() => {
  const context = new OfflineAudioContext(1, 1, 44100);
  const gainNode = new GainNode(context);
  assert_equals(gainNode.gain.defaultValue, 1,
      'GainNode.gain.defaultValue should be 1.');
}, 'AudioParam: defaultValue attribute value');

test(() => {
  const context = new OfflineAudioContext(1, 1, 44100);
  const gainNode = new GainNode(context);
  assert_readonly(gainNode.gain, 'defaultValue');
}, 'AudioParam: defaultValue is a read-only attribute');

test(() => {
  const context = new OfflineAudioContext(1, 1, 44100);
  const initialValue = -1;
  const gainNode = new GainNode(context, {
    gain: initialValue,
  });
  assert_equals(gainNode.gain.value, initialValue,
      'GainNode.gain.value should be initialized to the value ' +
      'from the constructor.');
}, 'AudioParam: value attribute is initialized correctly');
