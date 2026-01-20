#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TARGET="bt_headset_gui"

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

echo ""
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  🎧 Bluetooth Kulaklık Simülatörü${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

cd "$PROJECT_DIR"

# İlk kurulum kontrolü
if [ ! -f ".bt_headset_backup/changes.txt" ]; then
  log_info "İlk kurulum algılandı, setup çalıştırılıyor..."
  "$SCRIPT_DIR/setup.sh"
  echo ""
fi

# Derleme gerekli mi?
if [ ! -f "$TARGET" ] || [ "bt_headset_gui.c" -nt "$TARGET" ]; then
  log_info "Derleniyor..."
  make gui
  echo ""
fi

# Capability kontrolü
if ! getcap "$TARGET" 2>/dev/null | grep -q "cap_net"; then
  log_info "Capability ekleniyor..."
  sudo setcap 'cap_net_admin,cap_net_raw+eip' "$TARGET"
  log_ok "Capability eklendi"
fi

# Bluetooth açık mı?
if command -v bluetoothctl >/dev/null 2>&1; then
  if ! bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then
    log_info "Bluetooth açılıyor..."
    bluetoothctl power on 2>/dev/null || true
  fi
fi

echo ""
log_info "Program başlatılıyor..."
echo ""

# Çalıştır
exec "./$TARGET"
