# Guides for implementers

For people writing their own client or server that should play with ours,
or with the classic Ace of Spades 0.75 ones.

| | Client | Server |
| --- | --- | --- |
| **0.75** (and 0.76), the classic protocol public servers speak | [client-0.75.md](client-0.75.md) | [server-0.75.md](server-0.75.md) |
| **1.0**, the protocol of our server | [client-1.0.md](client-1.0.md) | [server-1.0.md](server-1.0.md) |

Each guide says what has to be implemented, in the order it is needed, and
what our client (`client/`) or server (`server/`) does about it, including
the places where ours still falls short of the rule. The 1.0 guides are
roadmaps into the full wire specification, [../protocol-1.0.md](../protocol-1.0.md).

Start with [client-0.75.md](client-0.75.md) even for 1.0: it defines the
coordinates, enumerations and game rules both protocols share.
