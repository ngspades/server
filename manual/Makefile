# The manual: Typst sources -> HTML (site/) and PDF. `make help` lists the targets.
CHAPTERS := $(shell sed -n 's/.*file: "\([^"]*\)".*/\1/p' chapters.typ)
TYPST := typst compile --features html --root .
SERVER ?= ../server/build/ngspades-server

.PHONY: all gen site pdf check api-test publish clean help

all: site pdf

help:
	@echo "gen       regenerate the API checklist, config tables, command table, coverage"
	@echo "site      HTML of every chapter and the landing page in site/"
	@echo "pdf       a PDF a chapter in site/pdf/ and site/ngspades-server-manual.pdf"
	@echo "check     API coverage, Lua examples, links, placeholders"
	@echo "api-test  run tests/*.lua on a built server (SERVER=path), refresh gen/verified.typ"
	@echo "publish   push site/ to the gh-pages branch"
	@echo "clean     remove site/"

gen:
	python3 tools/extract_api.py
	python3 tools/gen_config.py
	python3 tools/gen_commands.py
	python3 tools/check.py --gen

site: gen
	python3 tools/build_site.py

pdf: gen
	mkdir -p site/pdf
	@for c in $(CHAPTERS); do $(TYPST) --input chapter=$$c entry.typ site/pdf/$$c.pdf 2>/dev/null || exit 1; echo pdf $$c; done
	$(TYPST) book.typ site/ngspades-server-manual.pdf 2>/dev/null

check: site
	python3 tools/check.py

api-test:
	SERVER=$(abspath $(SERVER)) python3 tools/run_api_test.py

publish: site pdf check
	sh tools/publish.sh

clean:
	rm -rf site
