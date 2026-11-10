PLUGINS := $(notdir $(wildcard plugins/*))

all: $(PLUGINS)

$(PLUGINS):
	$(MAKE) -C plugins/$@

install-%:
	$(MAKE) -C plugins/$* install

.PHONY: all $(PLUGINS)
