
var OpusEncoder = require( './OpusEncoder' );

var rate = 48000;
var channels = 1;
var encoder = new OpusEncoder( rate, channels );
console.log(encoder.whoAmI('/Users/sirius/Node/node-opusfile/opusfile/examples/output.opus', '/Users/sirius/Node/node-opusfile/opusfile/examples/output.opus'));
