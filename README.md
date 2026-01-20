# 🎧 Bluetooth Kulaklık Simülatörü (GUI / Pasif Mod)

PC'nizi Bluetooth kulaklık gibi gösteren GTK3 tabanlı uygulama. Telefon bağlanır, PC pasif olarak kabul eder. Rehber ve son görüşmeler PBAP ile çekilir.

## ✨ Özellikler

- 🔌 Pasif Mod
- 🔗 Otomatik eşleşme ve bağlantı
- 📇 PBAP rehber
- 🕘 PBAP son görüşmeler
- 📞 Arama arayüzü
- 🔍 HFP kanalı otomatik SDP ile bulunur
- 📊 SCO MTU dinamik okunur

## 📋 Gereksinimler

- Linux (BlueZ)
- Bluetooth adaptörü
- bluez
- bluez-obexd
- libbluetooth-dev
- libdbus-1-dev
- libgtk-3-dev
- pkg-config

## 🔧 Kurulum

### Hızlı Kurulum (Tek Komut)
```bash
./scripts/run.sh
```

### Manuel Kurulum
```bash
make setup     # Bağımlılıkları kur
make gui       # Derle
make install   # Sisteme kur (sudo gerektirmez)
```

## ▶️ Çalıştırma

```bash
# Yerel çalıştırma (derleme sonrası capability ekle)
sudo setcap 'cap_net_admin,cap_net_raw+eip' ./bt_headset_gui
./bt_headset_gui

# Veya sistem kurulumu sonrası
bt_headset_gui
```

**Not:** Artık `sudo` gerekmez! Capability ile çalışır.

## 📱 Kullanım Akışı

1. Başlat → PC keşfedilebilir olur
2. Telefon Bluetooth ayarlarından PC'ye bağlanır
3. Rehber ve son görüşmeler çekilir

## 🔧 Makefile Komutları

| Komut | Açıklama |
|-------|----------|
| `make gui` | Derle |
| `make setup` | Bağımlılıkları kur |
| `make install` | Sisteme kur (/usr/local/bin) |
| `make uninstall` | Sistemden kaldır |
| `make clean` | Temizle |
| `make run` | Kur + derle + çalıştır |

## 🐛 Sorun Giderme

- **obexd bulunamadı** → `./scripts/run.sh` tekrar çalıştırın
- **Permission denied** → `sudo setcap 'cap_net_admin,cap_net_raw+eip' ./bt_headset_gui`
- **Bluetooth servisi** → `sudo systemctl enable --now bluetooth`

## 🔐 Güvenlik Notları

⚠️ Uyarı: Bu program deneyseldir. Kritik aramalar için telefonunuzu doğrudan kullanın.


