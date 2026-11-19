# ngnet: the connection layer

The code that carries a game's bytes between a client and a server, and lets
two players meet with a six character code. It is in this repository for now;
the next step is a repository of its own that this one takes as a submodule,
as it does `common/enet`. Nothing in it knows what a map, a player slot or
protocol 1.0 is.

## What is in it

```
netlib/
  netlib.cmake            the build entry a program includes (sources, include dir)
  include/ngnet/
    host.h                what the library asks of the program: a log line and
                          an event sink (ngnet_set_log, ngnet_set_event)
    transport.h           the client's one connection to a server: connect,
                          service (events), send, rtt, disconnect; the
                          implementations are structs of these functions
    conn.h                a server's view of one client: conn_t, send,
                          disconnect, rtt, bytes pending
    p2p.h                 join codes: generate, normalise, validate
  src/
    host.c                the two hooks
    transport_enet.c      the client's ENet connection
    p2p.c                 join code generation (CSPRNG on every OS)
```

Channel 0 is reliable and ordered, channel 1 unreliable (`NGNET_CHANNELS`). A
packet is the same bytes on every carrier; the game's framing (protocol 1.0)
lives in `common/proto10` and above this layer, never inside it.

## Using it

```cmake
include(path/to/netlib/netlib.cmake)
target_sources(app PRIVATE ${NGNET_COMMON_SOURCES} ${NGNET_ENET_CLIENT_SOURCES})
target_include_directories(app PRIVATE ${NGNET_INCLUDE_DIR})
```

```c
#include <ngnet/host.h>
ngnet_set_log(my_log);       /* optional: the default is stderr */
```

A client picks an implementation (`struct transport_impl`) and drives it with
the `transport_*` calls; a server fills a `conn_transport_t` per carrier and
hands each accepted client to the program as a `conn_t`.

## What is still in the game, and the plan

Moving a file here means it keeps no include of the game (`log.h`,
`eventlog.h`, `lang.h`, `window.h`, `network.h`, `config.h`). What is left to
move, each behind the same two hooks (plus a `tr()` hook where a player reads
the text):

| Piece | Where it is now | Blocked by |
| --- | --- | --- |
| driver (`transport_use_*`) | the client's `transport.c` | the demo transport (`demo.h`) |
| desktop WebRTC + signalling | the client's `transport_rtc.c` | `http.h`, `master.h`, `lang.h`, `window.h` |
| browser WebRTC | the client's `transport_webrtc.c`, the client's `net.js` | `master.h`, `lang.h` |
| Web Worker loopback | the client's `transport_worker.c`, the client's `workerbridge.js` | `lang.h` |
| connect tokens | the client's `connecttoken.c` | `http.h`, `master.h` |
| master client (list, offers) | the client's `master.c`, `Server/Net/Master.c` | game settings, the server struct |
| server ENet, WebRTC | `Server/Net/EnetConn.c`, `WebRtc.c`, `RtcPeer.cpp` | `Server/Structs`, `server_conn_*` |
| HTTP | the client's `http.c` (naett) | none: can move as it is |

`conn_t.player_id` is the program's opaque owner id; rename it to `owner` when
the server pieces move. The events the server's transports raise
(`server_conn_connect/receive/closed`) become a callback table
(`ngnet_server_events`) that the game fills.

## Rules for this folder

- Everything here is plain C (C++17 only where libdatachannel needs it), no
  header of the game, no game type.
- A new connection feature adds its public header under `include/ngnet/` and
  its documentation here in the same change.
- The game's own tests (the harness, `tools/rtcprobe`) are the library's tests
  until it has a repository: a change here runs them.
