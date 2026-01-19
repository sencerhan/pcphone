# Bluetooth Kulaklık Simülatörü Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread -lbluetooth -lpulse-simple -lpulse
DBUS_CFLAGS = $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS = $(shell pkg-config --libs dbus-1 2>/dev/null)
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

TARGET_GUI = bt_headset_gui
SRC_GUI = bt_headset_gui.c

.PHONY: all gui clean deps setup run help

all: gui

gui: $(TARGET_GUI)
	@echo ""
	@echo "✓ GUI versiyonu derlendi!"
	@echo "  Çalıştırmak için: sudo ./$(TARGET_GUI)"

$(TARGET_GUI): $(SRC_GUI)
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) $(GTK_CFLAGS) -o $@ $< $(LDFLAGS) $(DBUS_LIBS) $(GTK_LIBS)

deps: setup

setup:
	@./scripts/setup.sh

run:
	@./scripts/run.sh

clean:
	rm -f $(TARGET_GUI)
	@echo "✓ Temizlendi"

help:
	@echo ""
	@echo "Kullanılabilir komutlar:"
	@echo "  make gui    - GUI versiyonunu derle"
	@echo "  make setup  - Bağımlılıkları kur"
	@echo "  make run    - Kur + derle + çalıştır"
	@echo "  make clean  - Temizle"
