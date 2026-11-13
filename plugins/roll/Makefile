# Тонкая обёртка: сборка и установка делаются скриптами в scripts/.
.PHONY: all install uninstall test clean

all:
	./scripts/build.sh

# На Zynthian: sudo make install (собирает и ставит ссылки)
install:
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

# Тест движка (транспорт, Undo) и виджета вне Zynthian (виджету нужен дисплей)
test:
	mkdir -p build_test
	gcc -O1 -Wall $$(pkg-config --cflags lv2) -o build_test/test_transport test/test_transport.c -lm -pthread
	./build_test/test_transport
	python3 test/widget_preview.py

clean:
	rm -rf build build_test dist
