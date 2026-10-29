# PcPhone

Bluetooth phone for PC. The phone connects; the PC accepts.

## Build

```bash
make gui
sudo ./pc_phone_gui
```

## Install .desktop entry (tel: handler)

Create `~/.local/share/applications/pcphone.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=PcPhone
Comment=Bluetooth Phone
Exec=/usr/bin/pcphone %u
Icon=call-start
Terminal=false
Categories=Network;Telephony;
MimeType=x-scheme-handler/tel;
StartupNotify=true
StartupWMClass=pcphone
X-GNOME-Autostart-enabled=true
X-GNOME-UsesNotifications=true
```

Register and set as default handler:

```bash
update-desktop-database ~/.local/share/applications
xdg-mime default pcphone.desktop x-scheme-handler/tel
```

For system-wide install, place the file in `/usr/share/applications/` and run `sudo update-desktop-database`.

## Autostart

Enable via the "🚀 Autostart" checkbox in the app, or manually copy the same `.desktop` file to `~/.config/autostart/`.
```