# The server, its tests and its tools. `make help` lists the targets.
# The binary's name is set once in common/game.cmake.
SERVER_BIN := $(shell sed -n 's/^set(GAME_SERVER_NAME "\(.*\)")/\1/p' common/game.cmake)
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
CMAKE_EXTRA ?=

.PHONY: all server server-asan server-static test test-proto10 test-ext075 config-check smoke \
	probe-proto10 run run-075 clean help

all: server

help:
	@echo "server         Release build: server/build/$(SERVER_BIN)"
	@echo "server-asan    AddressSanitizer + UBSan build: server/build-asan/$(SERVER_BIN)"
	@echo "server-static  the release's static Linux binary (Docker, Alpine)"
	@echo "test           protocol 1.0's codec, the 0.75 extensions, both configs"
	@echo "smoke          every game mode played by bots under the sanitizers"
	@echo "probe-proto10  a protocol 1.0 client against the built server"
	@echo "run, run-075   start the server (1.0, or the 0.75 profile)"

server:
	cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release $(CMAKE_EXTRA)
	cmake --build server/build -j$(JOBS)

server-asan:
	cmake -S server -B server/build-asan -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
		-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" $(CMAKE_EXTRA)
	cmake --build server/build-asan -j$(JOBS)

server-static:
	docker run --rm -v "$(CURDIR):/src" -w /src alpine:3.22 tools/build_server_static.sh

test: test-proto10 test-ext075 config-check

test-proto10:
	cmake -S common/proto10 -B common/proto10/build
	cmake --build common/proto10/build -j$(JOBS)
	common/proto10/build/test_proto10

test-ext075:
	python3 server/tools/ext075test/run.py

config-check: server
	cd server && build/$(SERVER_BIN) --check-config
	cd server && build/$(SERVER_BIN) --profile 075 --check-config

smoke: server-asan
	tools/smoke_modes.sh server/build-asan/$(SERVER_BIN)

probe-proto10: server
	cmake -S tools/p10probe -B tools/p10probe/build -DP10PROBE_ENET_DIR=$(CURDIR)/common/enet
	cmake --build tools/p10probe/build -j$(JOBS)
	python3 tools/p10probe/run_probe.py --server server/build/$(SERVER_BIN) all

run: server
	cd server && SP_MASTER=0 build/$(SERVER_BIN)

run-075: server
	cd server && SP_MASTER=0 build/$(SERVER_BIN) --profile 075

clean:
	rm -rf server/build server/build-asan common/proto10/build tools/p10probe/build
