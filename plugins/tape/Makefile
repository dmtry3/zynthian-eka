# Плагин собирается и ставится скриптами в scripts/ (all, install: тонкие обёртки),
# test: офлайн-тесты ядра и обёртки LV2.
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti

CORE_H := $(wildcard core/*.h)
CORE_CPP := $(wildcard core/*.cpp)

.PHONY: all install uninstall test clean

all:
	./scripts/build.sh

# На Zynthian: sudo make install (собирает и ставит ссылки)
install:
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

test: build_test/test_core build_test/test_looper build_test/test_session build_test/test_plugin
	@mkdir -p tests/out
	./build_test/test_core
	./build_test/test_looper
	./build_test/test_session
	./build_test/test_plugin
	python3 tests/check_ttl.py

build_test/test_core: tests/test_core.cpp $(CORE_H)
	@mkdir -p build_test
	$(CXX) $(CXXFLAGS) -o $@ tests/test_core.cpp -lm

build_test/test_looper: tests/test_looper.cpp $(CORE_H) $(CORE_CPP)
	@mkdir -p build_test
	$(CXX) $(CXXFLAGS) -o $@ tests/test_looper.cpp $(CORE_CPP) -lm

build_test/test_session: tests/test_session.cpp $(CORE_H) $(CORE_CPP)
	@mkdir -p build_test
	$(CXX) $(CXXFLAGS) -o $@ tests/test_session.cpp $(CORE_CPP) -lm

# Обёртка LV2 без хоста (нужны заголовки lv2, как и для сборки плагина)
build_test/test_plugin: tests/test_plugin.cpp lv2/eka_tape.cpp $(CORE_H) $(CORE_CPP)
	@mkdir -p build_test
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags lv2) -o $@ tests/test_plugin.cpp $(CORE_CPP) -lm

clean:
	rm -rf build build_test tests/out
