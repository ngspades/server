// The browser's server's transport, its JavaScript half (an Emscripten
// --js-library; Net/Worker.c is the C half): the queue web/pre.js fills from
// the clients' MessagePorts, drained on the tick, and the sends. Called from
// the worker's main thread only (the tick), never from a pthread.

addToLibrary({
	// the next event: info[0] its kind, [1] its peer, [2] its channel, [3]
	// its length. 0 when none
	sd_srv_peek: function(info) {
		var W = Module.sdWorker;
		if(!W || !W.queue.length)
			return 0;
		var e = W.queue[0];
		HEAP32[info >> 2] = e.kind;
		HEAP32[(info >> 2) + 1] = e.peer;
		HEAP32[(info >> 2) + 2] = e.channel;
		HEAP32[(info >> 2) + 3] = e.data ? e.data.length : 0;
		return 1;
	},

	// takes the event peeked, its message copied into `into`
	sd_srv_pop: function(into) {
		var W = Module.sdWorker;
		if(!W || !W.queue.length)
			return;
		var e = W.queue.shift();
		if(into && e.data)
			HEAPU8.set(e.data, into);
	},

	// one message on the peer's channel: text (control) or a packet. A
	// packet is a copy (the wasm's memory is shared, which postMessage
	// cannot transfer) handed over without a second one
	sd_srv_send: function(peer, channel, data, length, text) {
		var W = Module.sdWorker;
		var p = W && W.peers[peer];
		if(!p || p.closed)
			return;
		var port = p.ports[channel] || p.ports[0];
		try {
			if(text) {
				port.postMessage(UTF8ToString(data, length));
			} else {
				var copy = HEAPU8.slice(data, data + length);
				port.postMessage(copy.buffer, [copy.buffer]);
			}
		} catch(e) {}
	},

	sd_srv_close: function(peer) {
		var W = Module.sdWorker;
		var p = W && W.peers[peer];
		if(!p)
			return;
		p.closed = true;
		delete W.peers[peer];
		// a moment later: the goodbye's text just posted must reach the
		// client before the ports are gone
		setTimeout(function() {
			p.ports.forEach(function(port) {
				try { port.close(); } catch(e) {}
			});
		}, 1000);
	},
});
