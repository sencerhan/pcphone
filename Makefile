# PcPhone Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2
CXXFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread -lbluetooth -lpulse-simple -lpulse
DBUS_CFLAGS = $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS = $(shell pkg-config --libs dbus-1 2>/dev/null)
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

DOMAIN = pcphone
LOCALEDIR ?= /usr/local/share/locale
LANGS = it
MO_TARGETS = $(foreach lang,$(LANGS),locale/$(lang)/LC_MESSAGES/$(DOMAIN).mo)

TARGET_GUI = pc_phone_gui
SRC_GUI = pc_phone_gui.c
OBJ_GUI = pc_phone_gui.o

WEBRTC_CFLAGS = $(shell pkg-config --cflags webrtc-audio-processing 2>/dev/null)
WEBRTC_LIBS = $(shell pkg-config --libs webrtc-audio-processing 2>/dev/null)

ifneq ($(strip $(WEBRTC_CFLAGS)),)
	CFLAGS += -DHAVE_WEBRTC_APM $(WEBRTC_CFLAGS)
	CXXFLAGS += -DHAVE_WEBRTC_APM $(WEBRTC_CFLAGS)
	LDFLAGS += $(WEBRTC_LIBS)
	SRC_GUI += audio_processing_wrapper.cpp
	OBJ_GUI += audio_processing_wrapper.o
endif

.PHONY: all gui clean deps setup run help translations install-translations

all: gui

locale/%/LC_MESSAGES/$(DOMAIN).mo: po/%.po
	@mkdir -p $(dir $@)
	msgfmt -o $@ $<

translations: $(MO_TARGETS)

gui: translations $(TARGET_GUI)
	@echo ""
	@echo "✓ GUI version built!"
	@echo "  Run with: sudo ./$(TARGET_GUI)"

$(TARGET_GUI): $(OBJ_GUI)
	$(CXX) -o $@ $(OBJ_GUI) $(LDFLAGS) $(DBUS_LIBS) $(GTK_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) $(GTK_CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DBUS_CFLAGS) $(GTK_CFLAGS) -c $< -o $@

deps: setup

setup:
	@./scripts/setup.sh

run:
	@./scripts/run.sh

clean:
	rm -f $(TARGET_GUI) $(OBJ_GUI)
	rm -rf locale
	@echo "✓ Cleaned"

install: $(TARGET_GUI) install-translations
	@echo "Installing application..."
	sudo cp $(TARGET_GUI) /usr/local/bin/
	sudo setcap 'cap_net_admin,cap_net_raw+eip' /usr/local/bin/$(TARGET_GUI)
	@echo "Registering tel: URI handler..."
	mkdir -p ~/.local/share/applications
	cp pc_phone_gui.desktop ~/.local/share/applications/
	xdg-mime default pc_phone_gui.desktop x-scheme-handler/tel
	update-desktop-database ~/.local/share/applications 2>/dev/null || true
	@echo "✓ Installation complete!"
	@echo "  • Run with 'pc_phone_gui'"
	@echo "  • Clicking tel: links in browsers will open it automatically"

install-translations: translations
	@for mo in $(MO_TARGETS); do \
		lang=$$(echo $$mo | cut -d/ -f2); \
		install -d $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES; \
		install -m 644 $$mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/$(DOMAIN).mo; \
		echo "✓ Installed $$mo -> $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/$(DOMAIN).mo"; \
	done

uninstall:
	@./scripts/uninstall.sh
	@rm -f ~/.local/share/applications/pc_phone_gui.desktop
	@update-desktop-database ~/.local/share/applications 2>/dev/null || true
	@echo "✓ tel: handler kaldırıldı"

help:
	@echo ""
	@echo "Available commands:"
	@echo "  make gui       - Build GUI version"
	@echo "  make setup     - Install dependencies + system setup"
	@echo "  make run       - One-click run (setup + build + run)"
	@echo "  make install   - Install system-wide (/usr/local/bin)"
	@echo "  make uninstall - Restore system to previous state"
	@echo "  make clean     - Clean build files"
