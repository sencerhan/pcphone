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

# main.conf geri yükle
restore_main_conf() {
  if [ -f "$BACKUP_DIR/main.conf.bak" ]; then
    log_info "main.conf geri yükleniyor..."
    sudo cp "$BACKUP_DIR/main.conf.bak" "$MAIN_CONF"
    sudo systemctl restart bluetooth 2>/dev/null || true
    log_ok "main.conf geri yüklendi"
  else
    log_warn "main.conf yedeği bulunamadı"
  fi
}

# Kullanıcıyı bluetooth grubundan çıkar
restore_group() {
  if [ -f "$BACKUP_DIR/changes.txt" ]; then
    if grep -q "bluetooth_group_added=true" "$BACKUP_DIR/changes.txt"; then
      log_info "Kullanıcı bluetooth grubundan çıkarılıyor..."
      sudo gpasswd -d "$USER" bluetooth 2>/dev/null || true
      log_ok "Kullanıcı bluetooth grubundan çıkarıldı"
    fi
  fi
}

# Binary'yi kaldır
remove_binary() {
  if [ -f "/usr/local/bin/bt_headset_gui" ]; then
    log_info "Sistem binary'si kaldırılıyor..."
    sudo rm -f /usr/local/bin/bt_headset_gui
    log_ok "Binary kaldırıldı"
  fi
}

# Derleme dosyalarını temizle
clean_build() {
  log_info "Derleme dosyaları temizleniyor..."
  cd "$PROJECT_DIR"
  rm -f bt_headset_gui *.o 2>/dev/null || true
  log_ok "Derleme dosyaları temizlendi"
}

# Backup klasörünü sil
remove_backup() {
  if [ -d "$BACKUP_DIR" ]; then
    log_info "Yedek dosyaları siliniyor..."
    rm -rf "$BACKUP_DIR"
    log_ok "Yedek dosyaları silindi"
  fi
}

# Kullanıcı verilerini sil (opsiyonel)
remove_user_data() {
  log_info "Kullanıcı verileri siliniyor..."
  cd "$PROJECT_DIR"
  rm -f settings.json contacts.csv recents.csv 2>/dev/null || true
  log_ok "Kullanıcı verileri silindi"
}

main() {
  echo ""
  echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo -e "${RED}  🗑️  Bluetooth Kulaklık Simülatörü - Kaldırma${NC}"
  echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo ""
  
  # Onay iste
  echo -e "${YELLOW}Bu işlem aşağıdakileri yapacak:${NC}"
  echo "  • Sistem binary'sini kaldır (/usr/local/bin)"
  echo "  • main.conf'u eski haline getir"
  echo "  • Bluetooth grup üyeliğini geri al"
  echo "  • Derleme dosyalarını temizle"
  echo ""
  
  read -p "Devam etmek istiyor musunuz? [e/H] " -n 1 -r
  echo ""
  
  if [[ ! $REPLY =~ ^[EeYy]$ ]]; then
    log_warn "İptal edildi"
    exit 0
  fi
  
  echo ""
  
  remove_binary
  restore_main_conf
  restore_group
  clean_build
  
  # Kullanıcı verilerini silmek ister mi?
  echo ""
  read -p "Kullanıcı verilerini de silmek ister misiniz? (settings.json, contacts.csv) [e/H] " -n 1 -r
  echo ""
  
  if [[ $REPLY =~ ^[EeYy]$ ]]; then
    remove_user_data
  fi
  
  remove_backup
  
  echo ""
  echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo -e "${GREEN}  ✓ Kaldırma tamamlandı!${NC}"
  echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo ""
  log_info "Bluetooth ayarlarının tam olarak geri dönmesi için yeniden başlatmanız önerilir."
  echo ""
}

main "$@"
