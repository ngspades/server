// The browser's server (docs/web.md phase 6): server.js runs as
// a Web Worker the page starts (the client's workerbridge.js), and this
// --pre-js is the worker's side of that bridge. It is in the module's own
// scope, before the runtime: FS, ENV and callMain are reached from the
// callbacks below, once the runtime is up.
//
// The page talks to it in messages:
// - {type: 'boot', argv, env, maps}: the server's arguments (the desktop's
//   listen server gets the same ones, the client's listenserver.c), the page's
//   SP_* variables, and the maps to put in Resources/maps first;
// - {type: 'peer', reliable, unreliable}: a client's two MessagePorts, the
//   WebRTC wire of docs/protocol-1.0.md 3.4 (Net/Worker.c).
// It answers {type: 'log', line} for every line the server prints,
// {type: 'started'} at "Server started", {type: 'failed', why} when the
// boot could not run, and {type: 'exit', code} when the server exits.
//
// The pthreads (the bots' planner, the ngmap encoder) load this same file in
// workers of their own: they take Emscripten's own messages, never these.

if(!ENVIRONMENT_IS_PTHREAD) {
	// every file the build asks for (the wasm) carries the worker's own
	// ?v= (the page's cache tag, the client's cachetag.cmake): a cache never
	// mixes this build's code with another's wasm
	Module.locateFile = function(path, dir) {
		return dir + path + self.location.search;
	};

	var sdWorkerStarted = false;
	function sdWorkerLine(line) {
		// the server colours its terminal's lines (Util/Log.h): a page's
		// console and gameLog want the text
		line = String(line).replace(/\x1B\[[0-9;]*m/g, '');
		postMessage({type: 'log', line: line});
		if(!sdWorkerStarted && line.indexOf('Server started') >= 0) {
			sdWorkerStarted = true;
			postMessage({type: 'started'});
		}
	}
	Module.print = sdWorkerLine;
	Module.printErr = sdWorkerLine;
	Module.onExit = function(code) {
		postMessage({type: 'exit', code: code});
	};
	Module.onAbort = function(what) {
		postMessage({type: 'exit', code: -1, why: String(what)});
	};

	// the queue Net/Worker.c drains on the tick (worker_lib.js): every
	// callback below only pushes
	Module.sdWorker = {queue: [], peers: {}, next: 1};

	var sdBoot = null, sdRuntimeUp = false, sdBooted = false;
	Module.onRuntimeInitialized = function() {
		sdRuntimeUp = true;
		sdWorkerBoot();
	};

	// the root the server runs in (--root): its config is written there from
	// the copy built into the binary (Main.c), its maps fetched below. What it
	// writes goes to --data, the worker's own memory: nothing of it is the
	// page's /user/ folder
	var SD_ROOT = '/srv';

	function sdMkdirs(path) {
		try {
			FS.mkdirTree(path);
		} catch(e) {}
	}

	// one map's files into Resources/maps/<name>/: the build's own copy next
	// to the page (server-maps/, the page's origin, so a match plays with no
	// network), else the maps repository's ngmap, which the server unpacks
	// itself (MapDownload.c's _unpack_local). 1 when the map is there
	function sdFetchMap(name) {
		if(!/^[A-Za-z0-9_.-]+$/.test(name))
			return Promise.resolve(false);
		var dir = SD_ROOT + '/Resources/maps/' + name;
		function get(url) {
			return fetch(url).then(function(r) {
				return r.ok ? r.arrayBuffer().then(function(b) { return new Uint8Array(b); }) : null;
			}, function() { return null; });
		}
		function put(file, data) {
			sdMkdirs(dir);
			FS.writeFile(dir + '/' + file, data);
		}
		var own = 'server-maps/' + name + '/' + name;
		var tag = self.location.search;
		return get(own + '.toml' + tag).then(function(toml) {
			if(toml) {
				// a built-in map ships as its .vxl, or as an ngmap alone (the
				// maps of the 2012 server list), which the server unpacks
				return Promise.all([get(own + '.vxl' + tag), get(own + '.txt' + tag), get(own + '.ngmap' + tag)]).then(function(r) {
					var map = r[0] || r[2];
					if(!map)
						return false;
					put(name + '.toml', toml);
					put(name + (r[0] ? '.vxl' : '.ngmap'), map);
					if(r[1])
						put(name + '.txt', r[1]);
					sdWorkerLine('Map ' + name + ': from the page (' + (map.length >> 10) + ' KB' + (r[0] ? '' : ' ngmap') + ')');
					return true;
				});
			}
			var repo = 'https://raw.githubusercontent.com/ngspades/maps/main/maps/' + name + '/' + name;
			return Promise.all([get(repo + '.toml'), get(repo + '.ngmap'), get(repo + '.txt')]).then(function(r) {
				if(!r[0] || !r[1])
					return false;
				put(name + '.toml', r[0]);
				put(name + '.ngmap', r[1]);
				if(r[2])
					put(name + '.txt', r[2]);
				sdWorkerLine('Map ' + name + ': from the maps repository (' + (r[1].length >> 10) + ' KB ngmap)');
				return true;
			});
		});
	}

	function sdWorkerBoot() {
		if(!sdBoot || !sdRuntimeUp || sdBooted)
			return;
		sdBooted = true;
		var boot = sdBoot;
		Object.keys(boot.env || {}).forEach(function(key) {
			ENV[key] = String(boot.env[key]);
		});
		sdMkdirs(SD_ROOT + '/Resources/maps');
		sdMkdirs('/data');
		Promise.all((boot.maps || []).map(sdFetchMap)).then(function(found) {
			found.forEach(function(ok, i) {
				if(!ok)
					sdWorkerLine('Map ' + boot.maps[i] + ' is not next to the page nor in the maps repository');
			});
			sdWorkerLine('Starting the server: ' + boot.argv.join(' '));
			try {
				callMain(['--root', SD_ROOT, '--data', '/data'].concat(boot.argv));
			} catch(e) {
				postMessage({type: 'failed', why: String(e)});
			}
		});
	}

	self.addEventListener('message', function(e) {
		var m = e.data;
		if(!m || typeof m !== 'object')
			return;
		if(m.type === 'boot') {
			sdBoot = m;
			sdWorkerBoot();
		} else if(m.type === 'peer') {
			sdWorkerPeer(m.reliable, m.unreliable);
		}
	});

	// a client's ports: each message queued as Net/Worker.c's event (1 open,
	// 2 text, 3 binary, 4 closed), the channel its port's (0 reliable, 1
	// unreliable)
	function sdWorkerPeer(reliable, unreliable) {
		var W = Module.sdWorker;
		var id = W.next++;
		var peer = {id: id, ports: [reliable, unreliable], closed: false};
		W.peers[id] = peer;
		W.queue.push({kind: 1, peer: id, channel: 0, data: null});
		var encoder = new TextEncoder();
		peer.ports.forEach(function(port, channel) {
			port.onmessage = function(ev) {
				if(peer.closed)
					return;
				var d = ev.data;
				if(typeof d === 'string')
					W.queue.push({kind: 2, peer: id, channel: channel, data: encoder.encode(d)});
				else if(d instanceof ArrayBuffer)
					W.queue.push({kind: 3, peer: id, channel: channel, data: new Uint8Array(d)});
			};
			// a port whose other end is gone (the page closed it: the
			// client left or the tab went), where the browser tells
			port.addEventListener('close', function() {
				if(!peer.closed)
					W.queue.push({kind: 4, peer: id, channel: channel, data: null});
			});
		});
	}
}
