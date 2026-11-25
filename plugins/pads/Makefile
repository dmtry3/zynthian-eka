# Thin wrapper: build, install, uninstall and tests are in scripts/.
.PHONY: all install uninstall test clean patch-smcpad

all:
	./scripts/build.sh

install:
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

patch-smcpad:
	./scripts/patch-smcpad.sh

test:
	./scripts/test.sh

clean:
	rm -rf build build_test dist
