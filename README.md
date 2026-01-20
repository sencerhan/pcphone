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

- Linux (Ubuntu 20.04+, Fedora, Arch)
- Bluetooth adaptörü (USB veya dahili)
- PulseAudio veya PipeWire

## 🚀 Tek Tıkla Kurulum ve Çalıştırma

```bash
./scripts/run.sh
```

Bu komut otomatik olarak:
- ✅ Gerekli paketleri kurar
- ✅ Bluetooth ayarlarını yapılandırır
- ✅ main.conf'u düzenler (yedek alır)
- ✅ Kullanıcıyı bluetooth grubuna ekler
- ✅ Programı derler
- ✅ Capability ekler
- ✅ Programı başlatır

## 🗑️ Temiz Kaldırma

```bash
make uninstall
# veya
./scripts/uninstall.sh
```

Bu komut:
- ✅ Sistem binary'sini kaldırır
- ✅ main.conf'u eski haline getirir
- ✅ Bluetooth grup üyeliğini geri alır
- ✅ Derleme dosyalarını temizler
- ✅ (Opsiyonel) Kullanıcı verilerini siler

## 📱 İlk Kullanım

1. `./scripts/run.sh` çalıştırın
2. Telefonunuzun Bluetooth ayarlarından PC'yi bulun
3. Eşleştirin ve "Handsfree" olarak bağlayın
4. Programda "Başlat" butonuna basın

**Not:** İlk kurulumdan sonra oturumu kapatıp açmanız gerekebilir (bluetooth grubu için).

## 🔧 Manuel Komutlar

| Komut | Açıklama |
|-------|----------|
| `make run` | Tek tıkla çalıştır |
| `make gui` | Sadece derle |
| `make setup` | Sadece kurulum yap |
| `make install` | Sisteme kur (/usr/local/bin) |
| `make uninstall` | Temiz kaldır (ayarları geri al) |
| `make clean` | Derleme dosyalarını temizle |

## 🐛 Sorun Giderme

| Sorun | Çözüm |
|-------|-------|
| Permission denied | `newgrp bluetooth` veya oturumu yeniden aç |
| Telefon bağlanmıyor | `bluetoothctl` ile discoverable on |
| SCO bağlantısı başarısız | Telefonu yeniden eşleştirin |
| Ses gelmiyor | PulseAudio Bluetooth modülünü kontrol edin |

### Bluetooth Durumunu Kontrol Et
```bash
# Adaptör durumu
hciconfig

# Bağlı cihazlar
bluetoothctl devices Connected

# Servis durumu
systemctl status bluetooth
```

## 📁 Dosya Yapısı

```
blue/
├── bt_headset_gui.c       # Ana uygulama
├── Makefile               # Derleme komutları
├── scripts/
│   ├── run.sh             # Tek tıkla çalıştır
│   ├── setup.sh           # Kurulum (backup alır)
│   └── uninstall.sh       # Kaldır (backup'tan geri yükler)
├── .bt_headset_backup/    # Otomatik yedekler
│   ├── main.conf.bak      # Orijinal Bluetooth ayarları
│   └── changes.txt        # Yapılan değişiklikler
├── settings.json          # Kullanıcı ayarları (sütun genişlikleri)
├── contacts.csv           # Rehber önbelleği
└── recents.csv            # Son aramalar önbelleği
```

## 🔐 Güvenlik Notları

⚠️ **Uyarı:** Bu program deneyseldir. Kritik aramalar için telefonunuzu doğrudan kullanın.


