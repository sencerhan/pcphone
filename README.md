# 🎧 Bluetooth Kulaklık Simülatörü (GUI / Pasif Mod)

PC'nizi Bluetooth kulaklık gibi gösteren GTK3 tabanlı uygulama. Telefon bağlanır, PC pasif olarak kabul eder. Rehber ve son görüşmeler PBAP ile çekilir.

## ✨ Özellikler

- 🔌 Pasif Mod
- 🔗 Otomatik eşleşme ve bağlantı
- 📇 PBAP rehber
- 🕘 PBAP son görüşmeler
- 📞 Arama arayüzü

## 📋 Gereksinimler

- Linux (BlueZ)
- Bluetooth adaptörü
- Root yetkisi (sudo)
- bluez
- bluez-obexd
- libbluetooth-dev
- libdbus-1-dev
- libgtk-3-dev
- pkg-config

## 🔧 Kurulum (Tek Komut)

Komut: ./scripts/run.sh

## ▶️ Çalıştırma

Komut: sudo ./bt_headset_gui

## 📱 Kullanım Akışı

1. Başlat → PC keşfedilebilir olur
2. Telefon Bluetooth ayarlarından PC’ye bağlanır
3. Rehber ve son görüşmeler çekilir

## 🐛 Sorun Giderme

obexd bulunamadı → scripts/run.sh tekrar çalıştırın.
Permission denied → sudo ./bt_headset_gui
Bluetooth servisi → sudo systemctl enable --now bluetooth

## 🔐 Güvenlik Notları



⚠️ Uyarı: Bu program deneyseldir. Kritik aramalar için telefonunuzu doğrudan kullanın.


