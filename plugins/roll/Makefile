# Тонкая обёртка: сборка и установка делаются скриптами в scripts/.
.PHONY: all install uninstall test clean

all:
	./scripts/build.sh

# На Zynthian: sudo make install (собирает и ставит ссылки)
install:
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

# Виджет вне Zynthian (нужен дисплей)
test:
	python3 test/widget_preview.py

clean:
	rm -rf build build_test dist
