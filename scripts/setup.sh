#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BACKUP_DIR="$PROJECT_DIR/.bt_headset_backup"
MAIN_CONF="/etc/bluetooth/main.conf"

# Renk kodları
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${BLUE}ℹ️  $1${NC}"; }
log_ok() { echo -e "${GREEN}✓ $1${NC}"; }
log_warn() { echo -e "${YELLOW}⚠️  $1${NC}"; }
log_error() { echo -e "${RED}❌ $1${NC}"; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1
}

# Paket kurulumu
install_packages() {
  log_info "Paketler kuruluyor..."
  
  if require_cmd apt-get; then
    sudo apt-get update -y
    sudo apt-get install -y \
      bluez \
      bluez-obexd \
      dbus-x11 \
      libbluetooth-dev \
      libdbus-1-dev \
      libgtk-3-dev \
      libpulse-dev \
      pkg-config \
      gcc \
      g++ \
      make
  elif require_cmd dnf; then
    sudo dnf install -y \
      bluez \
      bluez-obexd \
      bluez-libs \
      bluez-libs-devel \
      dbus-devel \
      gtk3-devel \
      pulseaudio-libs-devel \
      pkgconf-pkg-config \
      gcc \
      g++ \
      make
  elif require_cmd pacman; then
    sudo pacman -Sy --noconfirm \
      bluez \
      bluez-utils \
      libdbus \
      gtk3 \
      libpulse \
      pkgconf \
      gcc \
      make
  else
    log_error "Desteklenmeyen paket yöneticisi!"
    exit 1
  fi
  
  log_ok "Paketler kuruldu"
}

# Backup oluştur
create_backup() {
  mkdir -p "$BACKUP_DIR"
  
  # main.conf backup
  if [ -f "$MAIN_CONF" ]; then
    if [ ! -f "$BACKUP_DIR/main.conf.bak" ]; then
      sudo cp "$MAIN_CONF" "$BACKUP_DIR/main.conf.bak"
      log_ok "main.conf yedeklendi"
    fi
  fi
  
  # Mevcut grup üyeliğini kaydet
  if ! groups "$USER" | grep -q bluetooth; then
    echo "bluetooth_group_added=true" > "$BACKUP_DIR/changes.txt"
  else
    echo "bluetooth_group_added=false" > "$BACKUP_DIR/changes.txt"
  fi
}

# Bluetooth yapılandır
configure_bluetooth() {
  log_info "Bluetooth yapılandırılıyor..."
  
  if [ -f "$MAIN_CONF" ]; then
    # [General] bölümü yoksa ekle
    if ! grep -q "^\[General\]" "$MAIN_CONF"; then
      echo -e "\n[General]" | sudo tee -a "$MAIN_CONF" > /dev/null
    fi
    
    # Class ayarı
    if grep -q "^Class" "$MAIN_CONF"; then
      sudo sed -i 's/^Class.*/Class = 0x240404/' "$MAIN_CONF"
    elif grep -q "^#Class" "$MAIN_CONF"; then
      sudo sed -i 's/^#Class.*/Class = 0x240404/' "$MAIN_CONF"
    else
      sudo sed -i '/^\[General\]/a Class = 0x240404' "$MAIN_CONF"
    fi
    
    # DiscoverableTimeout
    if grep -q "^DiscoverableTimeout" "$MAIN_CONF"; then
      sudo sed -i 's/^DiscoverableTimeout.*/DiscoverableTimeout = 0/' "$MAIN_CONF"
    elif grep -q "^#DiscoverableTimeout" "$MAIN_CONF"; then
      sudo sed -i 's/^#DiscoverableTimeout.*/DiscoverableTimeout = 0/' "$MAIN_CONF"
    else
      sudo sed -i '/^\[General\]/a DiscoverableTimeout = 0' "$MAIN_CONF"
    fi
    
    # JustWorksRepairing
    if grep -q "^JustWorksRepairing" "$MAIN_CONF"; then
      sudo sed -i 's/^JustWorksRepairing.*/JustWorksRepairing = always/' "$MAIN_CONF"
    elif grep -q "^#JustWorksRepairing" "$MAIN_CONF"; then
      sudo sed -i 's/^#JustWorksRepairing.*/JustWorksRepairing = always/' "$MAIN_CONF"
    else
      sudo sed -i '/^\[General\]/a JustWorksRepairing = always' "$MAIN_CONF"
    fi
    
    log_ok "main.conf güncellendi"
  fi
  
  # Kullanıcıyı bluetooth grubuna ekle
  if getent group bluetooth >/dev/null 2>&1; then
    if ! groups "$USER" | grep -q bluetooth; then
      sudo usermod -aG bluetooth "$USER"
      log_ok "Kullanıcı bluetooth grubuna eklendi"
    fi
  fi
}

# Bluetooth servisini başlat
start_bluetooth() {
  log_info "Bluetooth servisi başlatılıyor..."
  
  if require_cmd systemctl; then
    sudo systemctl enable bluetooth 2>/dev/null || true
    sudo systemctl restart bluetooth 2>/dev/null || true
    log_ok "Bluetooth servisi başlatıldı"
  fi
}

# PC'yi eşleştirmeye hazırla
prepare_pairing() {
  log_info "Eşleştirme modu açılıyor..."
  
  # bluetoothctl ile otomatik ayarlar
  if require_cmd bluetoothctl; then
    bluetoothctl power on 2>/dev/null || true
    bluetoothctl discoverable on 2>/dev/null || true
    bluetoothctl pairable on 2>/dev/null || true
    log_ok "PC keşfedilebilir modda"
  fi
}

main() {
  echo ""
  echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo -e "${BLUE}  🎧 Bluetooth Kulaklık Simülatörü - Kurulum${NC}"
  echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo ""
  
  install_packages
  create_backup
  configure_bluetooth
  start_bluetooth
  prepare_pairing
  
  echo ""
  echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo -e "${GREEN}  ✓ Kurulum tamamlandı!${NC}"
  echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo ""
  
  # Grup değişikliği varsa uyar
  if [ -f "$BACKUP_DIR/changes.txt" ] && grep -q "bluetooth_group_added=true" "$BACKUP_DIR/changes.txt"; then
    log_warn "Bluetooth grubu için oturumu kapatıp açın veya:"
    echo "    newgrp bluetooth"
    echo ""
  fi
}

main "$@"
