# 🎧 Bluetooth Headset Simulator (GUI / Passive Mode)

GTK3-based application that makes your PC appear as a Bluetooth headset. The phone connects, PC accepts passively. Contacts and recent calls are fetched via PBAP.

## ✨ Features

- 🔗 Automatic pairing and connection
- 📇 PBAP contacts
- 🕘 PBAP recent calls
- 📞 Call interface
- 🔍 HFP channel automatically found via SDP
- 📊 SCO MTU dynamically read

## 📋 Requirements

- Linux (Ubuntu 20.04+, Fedora, Arch)
- Bluetooth adapter (USB or built-in)
- PulseAudio or PipeWire

## � Snap Installation (Recommended)

```bash
sudo snap install pcphone --classic
```

### ⚠️ Important: WirePlumber Configuration

If you use PipeWire (Ubuntu 22.04+), you must disable WirePlumber's HFP handling for PcPhone to work:

```bash
mkdir -p ~/.config/wireplumber/bluetooth.lua.d
echo 'bluez_monitor.properties = { ["bluez5.headset-roles"] = "[ ]" }' > ~/.config/wireplumber/bluetooth.lua.d/51-disable-hfp.lua
systemctl --user restart wireplumber
```

This allows PcPhone to control the HFP (Hands-Free Profile) connection directly.

## 🚀 One-Click Installation and Running (Manual)

```bash
./scripts/run.sh
```

This command automatically:
- ✅ Installs required packages
- ✅ Configures Bluetooth settings
- ✅ Edits main.conf (takes backup)
- ✅ Adds user to bluetooth group
- ✅ Compiles the program
- ✅ Adds capabilities
- ✅ Starts the program

## 🗑️ Clean Uninstall

```bash
make uninstall
# or
./scripts/uninstall.sh
```

This command:
- ✅ Removes system binary
- ✅ Restores main.conf to original
- ✅ Revokes Bluetooth group membership
- ✅ Cleans build files
- ✅ (Optional) Deletes user data

## 📱 First Use

1. Run `./scripts/run.sh`
2. Find the PC in your phone's Bluetooth settings
3. Pair and connect as "Handsfree"
4. Press the "Start" button in the program

**Note:** After first installation, you may need to log out and back in (for bluetooth group).

## 🔧 Manual Commands

| Command | Description |
|---------|-------------|
| `make run` | One-click run |
| `make gui` | Compile only |
| `make setup` | Setup only |
| `make install` | Install to system (/usr/local/bin) |
| `make uninstall` | Clean uninstall (restore settings) |
| `make clean` | Clean build files |

## 🐛 Troubleshooting

| Issue | Solution |
|-------|----------|
| Permission denied | `newgrp bluetooth` or restart session |
| Phone not connecting | `bluetoothctl` set discoverable on |
| SCO connection failed | Re-pair the phone |
| No sound | Check PulseAudio Bluetooth module |

### Check Bluetooth Status
```bash
# Adapter status
hciconfig

# Connected devices
bluetoothctl devices Connected

# Service status
systemctl status bluetooth
```

## 📁 File Structure

```
blue/
├── pc_phone_gui.c       # Main application
├── Makefile               # Build commands
├── scripts/
│   ├── run.sh             # One-click run
│   ├── setup.sh           # Setup (takes backup)
│   └── uninstall.sh       # Uninstall (restore from backup)
├── .pc_phone_backup/    # Automatic backups
│   ├── main.conf.bak      # Original Bluetooth settings
│   └── changes.txt        # Changes made
├── settings.json          # User settings (column widths)
├── contacts.csv           # Contacts cache
└── recents.csv            # Recent calls cache
```

## 🔐 Security Notes

⚠️ **Warning:** This program is experimental. Use your phone directly for critical calls.
