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

ifneq ($(wildcard scripts/deploy-zynthian.sh),)
DEPLOY_TARGET ?= root@zynthian.local
DEPLOY_DIR ?= /root/zynthian-eka

deploy:
	bash scripts/deploy-zynthian.sh all "$(DEPLOY_TARGET)" "$(DEPLOY_DIR)"

deploy-%:
	bash scripts/deploy-zynthian.sh $* "$(DEPLOY_TARGET)" "$(DEPLOY_DIR)"

.PHONY: deploy
endif
