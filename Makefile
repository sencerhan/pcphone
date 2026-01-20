# Bluetooth Kulaklık Simülatörü Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2
CXXFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread -lbluetooth -lpulse-simple -lpulse
DBUS_CFLAGS = $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS = $(shell pkg-config --libs dbus-1 2>/dev/null)
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

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

.PHONY: all gui clean deps setup run help

all: gui

gui: $(TARGET_GUI)
	@echo ""
	@echo "✓ GUI versiyonu derlendi!"
	@echo "  Çalıştırmak için: sudo ./$(TARGET_GUI)"

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
	@echo "✓ Temizlendi"

install: $(TARGET_GUI)
	@echo "Uygulama kuruluyor..."
	sudo cp $(TARGET_GUI) /usr/local/bin/
	sudo setcap 'cap_net_admin,cap_net_raw+eip' /usr/local/bin/$(TARGET_GUI)
	@echo "tel: URI handler kaydediliyor..."
	mkdir -p ~/.local/share/applications
	cp pc_phone_gui.desktop ~/.local/share/applications/
	xdg-mime default pc_phone_gui.desktop x-scheme-handler/tel
	update-desktop-database ~/.local/share/applications 2>/dev/null || true
	@echo "✓ Kurulum tamamlandı!"
	@echo "  • 'pc_phone_gui' komutu ile çalıştırabilirsiniz"
	@echo "  • Tarayıcılarda tel: linklerine tıklayınca otomatik arar"

uninstall:
	@./scripts/uninstall.sh
	@rm -f ~/.local/share/applications/pc_phone_gui.desktop
	@update-desktop-database ~/.local/share/applications 2>/dev/null || true
	@echo "✓ tel: handler kaldırıldı"

help:
	@echo ""
	@echo "Kullanılabilir komutlar:"
	@echo "  make gui       - GUI versiyonunu derle"
	@echo "  make setup     - Bağımlılıkları kur + sistem ayarları"
	@echo "  make run       - Tek tıkla çalıştır (setup + derle + çalıştır)"
	@echo "  make install   - Sisteme kur (/usr/local/bin)"
	@echo "  make uninstall - Sistemi eski haline getir"
	@echo "  make clean     - Temizle"
