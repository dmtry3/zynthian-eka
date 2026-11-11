# Эксперимент: Eka Probe (проверка хранения файлов в LV2-пресетах). Не плагин набора, удалить после проверки.
LV2_DIR ?= /usr/local/lib/lv2
B := build/eka-probe.lv2
.PHONY: all install uninstall clean

all:
	mkdir -p $(B)
	cp lv2/manifest.ttl lv2/eka_probe.ttl $(B)/
	gcc -O2 -fPIC -shared -Wall -Wextra $$(pkg-config --cflags lv2) -o $(B)/eka_probe.so eka_probe.c

# На Zynthian: sudo make install
install: all
	mkdir -p $(LV2_DIR)
	ln -sfn $(CURDIR)/$(B) $(LV2_DIR)/eka-probe.lv2

uninstall:
	rm -f $(LV2_DIR)/eka-probe.lv2

clean:
	rm -rf build
