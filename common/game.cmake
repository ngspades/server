# The game's name, in one place. Both builds (client/ and server/) include
# this file: the C code sees the strings as macros, CMake uses the variables
# (package name, server binary). tools/ scripts and the Makefile parse this
# file for the same values. Change it here and rebuild both.
set(GAME_NAME "Nexgen Spades")
# the mark, for where the full name does not fit: an app icon's label (a
# phone truncates it), the home screen of a narrow window, a splash. It
# never replaces the name in a sentence or on a store page
set(GAME_SHORT_NAME "NGS")
# the same mark for a template that is XML (Info.plist): CMake substitutes
# the raw text, and an unescaped "&" makes the plist unreadable
string(REPLACE "&" "&amp;" GAME_SHORT_NAME_XML "${GAME_SHORT_NAME}")
# file and folder names: archives, the player's folder, the app's files
set(GAME_SLUG "ngspades")
# the server's own name, apart from the game's: the binary, its archives, its
# Docker image and the service a host installs. Dedicated servers outlive a
# change of the game's working title (and a host's firewall rule, systemd unit
# and bookmarks name it), so they carry the organization's name instead of the
# slug: tools and workflows parse this line, never GAME_SLUG + "-server"
set(GAME_SERVER_NAME "ngspades-server")
set(GAME_SERVER_BINARY "${GAME_SERVER_NAME}")
# what the server calls itself in its welcome, its user agent, the server list
# and the {game} of config.toml: the organization's name, not the client's
set(GAME_SERVER_TITLE "ngspades-server")
# the game's id, the reverse-DNS name a store and a phone know an app by: the
# iOS bundle identifier (client/ios/ios.cmake), the Android application id
# (client/android/app/build.gradle, which reads it from here) and the Flatpak
# app id (packaging/flatpak, repeated there: a build recipe cannot read this
# file). It cannot change once a store has taken a release
set(GAME_APP_ID "pw.obex.ngspades")
# our site: the downloads, the news and the map of the game. The client
# points at it when our master server cannot be reached (a moved master
# leaves every installed copy without a list until it is updated)
set(GAME_WEBSITE_URL "https://ngspades.obex.pw")
# where players look for news and new versions: the client points at it
# when our master server cannot be reached (a moved master leaves every
# installed copy without a list until it is updated)
set(GAME_GITHUB_URL "https://github.com/ngspades")
# the Discord application of the Rich Presence (the client's rpc.c): its
# "Application ID" on discord.com/developers, digits only. Empty: no
# presence. Its Rich Presence art needs an asset named "logo"
set(GAME_DISCORD_CLIENT_ID "1563287726793564200")
add_compile_definitions(GAME_NAME="${GAME_NAME}" GAME_SHORT_NAME="${GAME_SHORT_NAME}" GAME_SLUG="${GAME_SLUG}"
	GAME_SERVER_BINARY="${GAME_SERVER_BINARY}" GAME_SERVER_TITLE="${GAME_SERVER_TITLE}" GAME_WEBSITE_URL="${GAME_WEBSITE_URL}"
	GAME_GITHUB_URL="${GAME_GITHUB_URL}")
