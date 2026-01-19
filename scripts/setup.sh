#!/usr/bin/env bash
set -euo pipefail

require_cmd() {
  command -v "$1" >/dev/null 2>&1
}

install_with_apt() {
  sudo apt-get update -y
  sudo apt-get install -y \
    bluez \
    bluez-obexd \
    dbus-x11 \
    libbluetooth-dev \
    libdbus-1-dev \
    libgtk-3-dev \
    pkg-config
}

install_with_dnf() {
  sudo dnf install -y \
    bluez \
    bluez-obexd \
    bluez-libs \
    bluez-libs-devel \
    dbus-devel \
    gtk3-devel \
    pkgconf-pkg-config
}

install_with_pacman() {
  sudo pacman -Sy --noconfirm \
    bluez \
    bluez-utils \
    libdbus \
    gtk3 \
    pkgconf
}

main() {
  if require_cmd apt-get; then
    install_with_apt
  elif require_cmd dnf; then
    install_with_dnf
  elif require_cmd pacman; then
    install_with_pacman
  else
    echo "Unsupported package manager. Please install BlueZ, bluez-obexd, GTK3, dbus, and headers manually." >&2
    exit 1
  fi

  if require_cmd systemctl; then
    sudo systemctl enable --now bluetooth || true
  fi

  echo "✓ Dependencies installed"
}

main
