PLUGINS := $(notdir $(wildcard plugins/*))

all: $(PLUGINS)

$(PLUGINS):
	$(MAKE) -C plugins/$@

# make install / make uninstall: все плагины; install-<плагин> / uninstall-<плагин>: один
install: $(addprefix install-,$(PLUGINS))

uninstall: $(addprefix uninstall-,$(PLUGINS))

install-%:
	$(MAKE) -C plugins/$* install

uninstall-%:
	$(MAKE) -C plugins/$* uninstall

.PHONY: all install uninstall $(PLUGINS)
