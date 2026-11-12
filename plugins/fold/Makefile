# Тонкая обёртка: сборка и установка делаются скриптами в scripts/.
.PHONY: all install uninstall test clean

all:
	./scripts/build.sh

# На Zynthian: sudo make install (собирает и ставит ссылки)
install:
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

test:
	./scripts/test.sh

clean:
	rm -rf build build_obj build_test dist
