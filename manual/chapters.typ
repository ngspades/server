// The chapters, in reading order. `file` is chapters/<file>.typ, also the
// HTML page's name; `part` groups them in the navigation.
#let chapters = (
  (file: "01-overview", title: "What the server is", part: "User manual"),
  (file: "02-install", title: "Installing and first start", part: "User manual"),
  (file: "03-configuration", title: "Configuration", part: "User manual"),
  (file: "04-modes-maps", title: "Game modes and maps", part: "User manual"),
  (file: "05-bots", title: "Bots", part: "User manual"),
  (file: "06-administration", title: "Administration and commands", part: "User manual"),
  (file: "07-network", title: "Network, the public list, browsers", part: "User manual"),
  (file: "08-operations", title: "Operations, logs and troubleshooting", part: "User manual"),
  (file: "09-plugins-lifecycle", title: "Plugins: how they run", part: "Lua plugin reference"),
  (file: "10-plugins-events", title: "Plugin events", part: "Lua plugin reference"),
  (file: "11-api-game", title: "The game table", part: "Lua plugin reference"),
  (file: "12-api-player", title: "Players and returned tables", part: "Lua plugin reference"),
  (file: "13-api-modes-bots", title: "Game modes and bots in Lua", part: "Lua plugin reference"),
  (file: "14-appendix", title: "Appendix: coverage and notes", part: "Lua plugin reference"),
)
