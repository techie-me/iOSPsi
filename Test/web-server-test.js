var spawn = require('child_process').spawn;
var fs = require('fs');
var Q = require('q');
var _ = require('underscore');
var request = require('./tunneled-request');
var SSHTunnel = require('./ssh-tunnel');


var SAMPLE_SIZE = 10, PROCESS_COUNT = 1;
var TIMEOUT = 30000;
var i;

/* Not spawning child processes for now, but I'll leave the code
if (!_.include(process.argv, 'childproc')) {
  for (i = 0; i < PROCESS_COUNT; i++) {
    var child = spawn(process.argv[0], process.argv.slice(1).concat('childproc'));
    child.on('exit', function(code) {
      console.log('childproc exited with', code);
    });

    child.stdout.on('data', function(data) {
      console.log('childproc said:', data.toString());
    });
  }
  return;
}
*/

var args = process.argv.splice(2);

// Load the connection info from the file written by the client.
var testConf = JSON.parse(fs.readFileSync('test-conn-info.json'));

// Use the "Testing" propagation channel
if (args.length === 0 || args[0].length !== 16) {
  console.log('Usage: test propagation_channel_id required');
  process.exit(1);
}
testConf.propagation_channel_id = args[0];

testConf.plonk = '../Client/psiclient/3rdParty/plonk.exe';
testConf.polipo = '../Client/psiclient/3rdParty/polipo.exe';


function mylog() {
  console.log.apply(console, arguments);
  console.log('');
}

// This is super rough
function makeFilename(/*...*/) {
  var i, s = '';
  for (i = 0; i < arguments.length; i++) {
    s += encodeURIComponent(arguments[i].toString()+';');
  }
  return s;
}

function outputServerReqResults(serverReqOptions, results) {
  console.log(
    serverReqOptions.reqType + ' test: ' +
    (serverReqOptions.tunneled ? '' : 'not ') + 'tunneled: ' +
    (serverReqOptions.parallel ? 'parallel: ' : 'serial: ') +
    '\n  ',
    results);
  console.log(''); // empty line
  writeServerReqResultsToCSV(serverReqOptions.parallel, serverReqOptions.reqType,
                             serverReqOptions.tunneled, results);
}

function writeServerReqResultsToCSV(
            parallel,
            reqType,
            tunneled,
            results) {
  var filename = makeFilename(parallel, reqType, tunneled) + '.csv';
  var csv = '"' + _.values(results).join('","') + '"\n';
  fs.appendFile(filename, csv);
}

function outputSiteReqResults(siteReqOptionsClone, results) {
  console.log(
    (siteReqOptionsClone.tunneled ? '' : 'not ') + 'tunneled request: ' +
    (siteReqOptionsClone.parallel ? 'parallel: ' : 'serial: ') +
    (siteReqOptionsClone.useHttps ? 'https://' : 'http://') +
    siteReqOptionsClone.host + siteReqOptionsClone.path +
    '\n  ',
    results);
  console.log(''); // empty line
}

// Call like
//   seq = seq.then(delayNext);
function delayNext() {
  var deferred = Q.defer();
  var delay = 1000;
  setTimeout(function() {
    mylog('delay', delay, 'ms');
    deferred.resolve();
  }, delay);
  return deferred.promise;
}

function makeServerRequestAndOutputFn(serverReqOptions) {
  var serverReqOptionsClone = _.clone(serverReqOptions);
  var fn = function() {
    return request.testServerRequest(serverReqOptionsClone)
              .then(function(results) {
                outputServerReqResults(serverReqOptionsClone, results);
                return Q.resolve();
              });
  };
  return fn;
}

function makeSiteRequestAndOutputFn(siteReqOptions) {
  var siteReqOptionsClone = _.clone(siteReqOptions);
  var fn = function() {
    return request.testSiteRequest(siteReqOptionsClone)
              .then(function(results) {
                outputSiteReqResults(siteReqOptionsClone, results);
                return Q.resolve();
              });
  };
  return fn;
}

var nextLocalPort = 10000;

// testConf will be modified with the `socks_proxy_port` and `http_proxy_port`
// used, and a `disconnect()` function which tears down the tunnel.
// The returned promise will be resolved when the connection is established.
function createTunnel(testConf) {
  var deferred = Q.defer();

  testConf.socks_proxy_port = nextLocalPort++;
  testConf.http_proxy_port = nextLocalPort++;

  var sshTunnel = new SSHTunnel(testConf);

  sshTunnel.once('connected', function() { deferred.resolve(); });

  sshTunnel.once('exit', function(unexpected) {
    sshTunnel.removeAllListeners();
    if (!deferred.promise.isResolved()) {
      // unexpected ought to be true, but we won't bother checking
      deferred.reject(new Error('unexpected disconnect', testConf.socks_proxy_port, testConf.http_proxy_port));
    }
  });

  sshTunnel.connect();

  // Set the disconnect function
  testConf.disconnect = _.bind(sshTunnel.disconnect, sshTunnel);

  return deferred.promise;
}

// Start out with a resolved promise
var seq = Q.resolve();

var serverReqOptions = {
  reqType: 'handshake',
  tunneled: false,
  parallel: true,
  testConf: testConf,
  count: SAMPLE_SIZE
};

var testNum = 1;


serverReqOptions.count = 5;
var tunnels = [];
var tunnel;
for (i = 0; i < 5; i++) {
  tunnel = { testConf: _.clone(testConf), promise: null };
  tunnels.push(tunnel);

  tunnel.testConf.socks_proxy_port = nextLocalPort++;
  tunnel.testConf.http_proxy_port = nextLocalPort++;
  tunnel.testConf.ossh = true;

  tunnel.promise = createTunnel(tunnel.testConf);
}

var requests = [];
Q.all(_.pluck(tunnels, 'promise')).then(function() {
  // All the tunnels are connected
  console.log('all tunnels connected\n');

  _.each(tunnels, function(tunnel) {
    serverReqOptions.testConf = tunnel.testConf;
    serverReqOptions.reqType = 'connected';
    serverReqOptions.testConf = request.addHexInfoToReq(serverReqOptions.testConf, testNum++);
    serverReqOptions.tunneled = true;
    makeServerRequestAndOutputFn(serverReqOptions)()
      .then(function() {
        tunnel.testConf.disconnect();
      }).end();
  });
});

return;



/*
for (i = 1; i <= 25; i++) {
  serverReqOptions.count = i*5;
  serverReqOptions.testConf = request.addHexInfoToReq(serverReqOptions.testConf, serverReqOptions.count);
  serverReqOptions.reqType = 'connected';

  serverReqOptions.tunneled = true;
  seq = seq.then(makeServerRequestAndOutputFn(serverReqOptions));
  seq = seq.then(delayNext);

  serverReqOptions.tunneled = false;
  seq = seq.then(makeServerRequestAndOutputFn(serverReqOptions));
  seq = seq.then(delayNext);
}

return;
*/


serverReqOptions.count = SAMPLE_SIZE;

serverReqOptions.reqType = 'handshake';
serverReqOptions.testConf = request.addHexInfoToReq(serverReqOptions.testConf, testNum++);
serverReqOptions.tunneled = true;
seq = seq.then(delayNext);
seq = seq.then(makeServerRequestAndOutputFn(serverReqOptions));
serverReqOptions.testConf = request.addHexInfoToReq(serverReqOptions.testConf, testNum++);
serverReqOptions.tunneled = false;
seq = seq.then(delayNext);
seq = seq.then(makeServerRequestAndOutputFn(serverReqOptions));

serverReqOptions.reqType = 'connected';
serverReqOptions.testConf = request.addHexInfoToReq(serverReqOptions.testConf, testNum++);
serverReqOptions.tunneled = true;
seq = seq.then(delayNext);
seq = seq.then(makeServerRequestAndOutputFn(serverReqOptions));
serverReqOptions.testConf = request.addHexInfoToReq(serverReqOptions.testConf, testNum++);
serverReqOptions.tunneled = false;
seq = seq.then(delayNext);
seq = seq.then(makeServerRequestAndOutputFn(serverReqOptions));

var siteReqOptions = {
  testConf: testConf,
  count: SAMPLE_SIZE,
  parallel: true,
  tunneled: true,
  httpsReq: true,
  host: 's3.amazonaws.com',
  path: '/0ubz-2q11-gi9y/en.html'
};

siteReqOptions.host = 's3.amazonaws.com';
siteReqOptions.tunneled = false;
seq = seq.then(delayNext);
seq = seq.then(makeSiteRequestAndOutputFn(siteReqOptions));
siteReqOptions.tunneled = true;
seq = seq.then(delayNext);
seq = seq.then(makeSiteRequestAndOutputFn(siteReqOptions));

siteReqOptions.host = '72.21.203.148';
siteReqOptions.tunneled = false;
seq = seq.then(delayNext);
seq = seq.then(makeSiteRequestAndOutputFn(siteReqOptions));
siteReqOptions.tunneled = true;
seq = seq.then(delayNext);
seq = seq.then(makeSiteRequestAndOutputFn(siteReqOptions));


seq.then(function() {
  mylog("all done");
}).end();

