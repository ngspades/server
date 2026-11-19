# The remote console

A TCP connection that runs the server's console commands, for a tool that
manages the server: [sadctl](https://github.com/ngspades/sadctl) (which
offers an MCP endpoint for AI agents on top of it), a monitor, a script.
Nothing else is in the server: no HTTP, no JSON-RPC, no tokens.

## Turning it on

`config.toml`:

```toml
[rcon]
enabled = true
bind = "127.0.0.1"
port = 32890          # the 0.75 profile's config uses 32900
password = "at least sixteen characters"
```

`SP_RCON_PASSWORD` (which also turns it on), `SP_RCON_PORT` and
`SP_RCON_BIND` win over the file. With a shorter password the server logs a
warning and leaves it off. It is not built on Windows.

The password is the only authentication and a console command runs with
admin rights, so **never open the port to the internet**: keep `bind` on
127.0.0.1 and use an SSH tunnel from another machine (`ssh -L
32890:127.0.0.1:32890 host`). The password travels in the clear.

## The wire

Text, one line at a time (1000 bytes at most).

1. Connect and send `auth <password>`. The answer is `ok`, or `denied` (after
   a second's pause) and the connection closes.
2. Send one command a line, with or without the leading `/`
   (`addbot 4 blue`, `/kick #3`, `/say hello`). The answer is what the
   command printed on the console (up to 64 KB), followed by an EOT byte
   (0x04) and a newline. A command that prints nothing answers `(done,
   nothing printed)`.
3. The line `status` is the exception: the answer is the server's state as one
   JSON object (name, map, mode, profile, uptime, score, limits, and every
   player with id, team, bot, alive, hp, kills, deaths, assists, captures,
   score, ip and ping).

`/shutdown`, `/login` and `/logout` are refused. A quiet connection is closed
after a minute. One client is served at a time; a line is run on the server's
tick (5 s at most to answer). Every line is written to the admin audit
(`logs/admin.log`, `actor=rcon`) and the event log (`EVT rcon`).

```sh
# a try, with netcat
(printf 'auth %s\nstatus\n' "$PW"; sleep 1) | nc 127.0.0.1 32890
```
