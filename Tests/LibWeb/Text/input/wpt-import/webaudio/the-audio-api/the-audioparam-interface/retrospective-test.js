// Create an audio graph on an offline context that consists of a
// constant source and two gain nodes. One of the nodes is the node te
// be tested and the other is the reference node.  The output from the
// test node is in channel 0 of the offline context; the reference
// node is in channel 1.
//
// Returns a dictionary with the context, source node, the test node,
// and the reference node.
function setupRetrospectiveGraph() {
  // Use a sample rate that is a power of two to eliminate round-off
  // in computing the currentTime.
  let context = new OfflineAudioContext(2, 16384, 16384);
  let source = new ConstantSourceNode(context);

  let test = new GainNode(context);
  let reference = new GainNode(context);

  source.connect(test);
  source.connect(reference);

  let merger = new ChannelMergerNode(
      context, {numberOfInputs: context.destination.channelCount});
  test.connect(merger, 0, 0);
  reference.connect(merger, 0, 1);

  merger.connect(context.destination);

  return {context: context, source: source, test: test, reference: reference};
}
