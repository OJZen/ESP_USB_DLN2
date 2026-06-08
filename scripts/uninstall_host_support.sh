#!/usr/bin/env bash
set -euo pipefail

INSTALL_LIB_DIR="/usr/local/lib/esp_usb_dln2"
INSTALL_LIBEXEC_DIR="/usr/local/libexec/esp_usb_dln2"
INSTALL_BIN="/usr/local/bin/esp-usb-dln2-check"
INSTALL_SBIN="/usr/local/sbin/esp-usb-dln2-uninstall"
CONFIG_DIR="/etc/esp_usb_dln2"
UDEV_RULE="/etc/udev/rules.d/99-zz-esp-usb-dln2.rules"
OLD_UDEV_RULE="/etc/udev/rules.d/99-esp-usb-dln2.rules"
SYSTEMD_UNIT="/etc/systemd/system/esp-usb-dln2-spi.service"
DKMS_PACKAGE="esp-usb-dln2-spi"
DKMS_VERSION="0.2.0"
DKMS_SOURCE_DIR="/usr/src/${DKMS_PACKAGE}-${DKMS_VERSION}"
SPI_MODULE_NAME="spi_dln2_spidev_test"

log() {
    printf 'esp_usb_dln2_uninstall: %s\n' "$*"
}

remove_file() {
    local path="$1"

    if [ -e "$path" ] || [ -L "$path" ]; then
        rm -f "$path"
        log "removed $path"
    fi
}

remove_dir_if_empty() {
    local path="$1"

    if [ -d "$path" ]; then
        rmdir "$path" 2>/dev/null || true
    fi
}

if [ "$(id -u)" -ne 0 ]; then
    printf 'uninstall_host_support.sh must run as root. Use sudo.\n' >&2
    exit 1
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop esp-usb-dln2-spi.service >/dev/null 2>&1 || true
fi

if lsmod | awk '{print $1}' | grep -qx "$SPI_MODULE_NAME"; then
    if rmmod "$SPI_MODULE_NAME"; then
        log "unloaded $SPI_MODULE_NAME"
    else
        log "could not unload $SPI_MODULE_NAME; a spidev user may still be active"
    fi
fi

if command -v dkms >/dev/null 2>&1; then
    dkms remove -m "$DKMS_PACKAGE" -v "$DKMS_VERSION" --all >/dev/null 2>&1 || true
fi

rm -rf "$DKMS_SOURCE_DIR"

remove_file "$UDEV_RULE"
remove_file "$OLD_UDEV_RULE"
remove_file "$SYSTEMD_UNIT"
remove_file "$INSTALL_BIN"
remove_file "$INSTALL_SBIN"
rm -rf "$INSTALL_LIB_DIR" "$INSTALL_LIBEXEC_DIR" "$CONFIG_DIR"

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi

if command -v depmod >/dev/null 2>&1; then
    depmod "$(uname -r)" || true
fi

remove_dir_if_empty /usr/local/libexec
remove_dir_if_empty /usr/local/lib

log "host support removed"
