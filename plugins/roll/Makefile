# Тонкая обёртка: сборка и установка делаются скриптами в scripts/.
.PHONY: all install uninstall clean

all:
	./scripts/build.sh

# На Zynthian: sudo make install (собирает и ставит ссылки)
install:
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

clean:
	rm -rf build build_test dist
