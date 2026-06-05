#!/usr/bin/env bash
set -euo pipefail

ACCESS_GROUP="${ESP_USB_DLN2_ACCESS_GROUP:-plugdev}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_RELEASE="$(uname -r)"
SPI_TOOL_DIR="$REPO_ROOT/tools/spi_test_device"
INSTALL_LIB_DIR="/usr/local/lib/esp_usb_dln2"
INSTALL_LIBEXEC_DIR="/usr/local/libexec/esp_usb_dln2"
CONFIG_DIR="/etc/esp_usb_dln2"
UDEV_RULE="/etc/udev/rules.d/99-zz-esp-usb-dln2.rules"
OLD_UDEV_RULE="/etc/udev/rules.d/99-esp-usb-dln2.rules"
SYSTEMD_UNIT="/etc/systemd/system/esp-usb-dln2-spi.service"
BUILD_DIR=""

cleanup() {
    if [ -n "$BUILD_DIR" ] && [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
}
trap cleanup EXIT

usage() {
    cat <<EOF
Usage: sudo $0 [--group GROUP]

Installs host support for the ESP USB DLN2 device:
  - udev permissions for GPIO/I2C/IIO/spidev
  - systemd oneshot service triggered when the USB device is plugged in
  - SPI spidev child registrar module for the current kernel
  - esp-usb-dln2-check readiness command
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --group)
            ACCESS_GROUP="${2:-}"
            [ -n "$ACCESS_GROUP" ] || { usage >&2; exit 2; }
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    printf 'install_host_support.sh must run as root. Use sudo.\n' >&2
    exit 1
fi

if ! getent group "$ACCESS_GROUP" >/dev/null 2>&1; then
    printf 'Group %s does not exist.\n' "$ACCESS_GROUP" >&2
    printf 'Create it first or rerun with --group GROUP.\n' >&2
    exit 1
fi

if [ ! -d "/lib/modules/$KERNEL_RELEASE/build" ]; then
    printf 'Missing kernel build directory: /lib/modules/%s/build\n' "$KERNEL_RELEASE" >&2
    printf 'Install kernel headers for the running kernel, then rerun this script.\n' >&2
    exit 1
fi

command -v make >/dev/null 2>&1 || {
    printf 'make is required to build the SPI helper module.\n' >&2
    exit 1
}
command -v systemctl >/dev/null 2>&1 || {
    printf 'systemctl is required for automatic plug-in setup.\n' >&2
    exit 1
}
command -v udevadm >/dev/null 2>&1 || {
    printf 'udevadm is required for device permission rules.\n' >&2
    exit 1
}

printf 'Building SPI helper for kernel %s...\n' "$KERNEL_RELEASE"
BUILD_DIR="$(mktemp -d /tmp/esp_usb_dln2_spi_helper.XXXXXX)"
install -m 0644 "$SPI_TOOL_DIR/Makefile" "$BUILD_DIR/Makefile"
install -m 0644 "$SPI_TOOL_DIR/spi_dln2_spidev_test.c" "$BUILD_DIR/spi_dln2_spidev_test.c"
make -C "$BUILD_DIR" all

install -Dm644 "$BUILD_DIR/spi_dln2_spidev_test.ko" \
    "$INSTALL_LIB_DIR/spi_dln2_spidev_test.ko"
install -Dm755 "$REPO_ROOT/scripts/esp_usb_dln2_setup_host_device.sh" \
    "$INSTALL_LIBEXEC_DIR/esp_usb_dln2_setup_host_device.sh"
install -Dm755 "$REPO_ROOT/scripts/check_device_ready.sh" \
    /usr/local/bin/esp-usb-dln2-check

install -d "$CONFIG_DIR"
sed "s/@ACCESS_GROUP@/$ACCESS_GROUP/g" "$REPO_ROOT/host_support/host.conf.in" \
    > "$CONFIG_DIR/host.conf"
chmod 0644 "$CONFIG_DIR/host.conf"

sed "s/@ACCESS_GROUP@/$ACCESS_GROUP/g" "$REPO_ROOT/host_support/99-zz-esp-usb-dln2.rules.in" \
    > "$UDEV_RULE"
chmod 0644 "$UDEV_RULE"
if [ "$OLD_UDEV_RULE" != "$UDEV_RULE" ]; then
    rm -f "$OLD_UDEV_RULE"
fi

install -Dm644 "$REPO_ROOT/host_support/esp-usb-dln2-spi.service" "$SYSTEMD_UNIT"

udevadm control --reload-rules
systemctl daemon-reload

printf 'Starting host setup for any currently connected ESP USB DLN2 device...\n'
if ! systemctl start esp-usb-dln2-spi.service; then
    printf 'Failed to start esp-usb-dln2-spi.service.\n' >&2
    printf 'Inspect logs with: journalctl -u esp-usb-dln2-spi.service -n 50 --no-pager\n' >&2
    exit 1
fi
for subsystem in usb platform gpio i2c-dev iio spidev; do
    udevadm trigger --subsystem-match="$subsystem" || true
done

cat <<EOF

Host support installed.

Access group: $ACCESS_GROUP
SPI helper:   $INSTALL_LIB_DIR/spi_dln2_spidev_test.ko
Config:       $CONFIG_DIR/host.conf
udev rule:    $UDEV_RULE
systemd unit: $SYSTEMD_UNIT

Run:
  esp-usb-dln2-check

If your user is not in group '$ACCESS_GROUP', add it and start a new login shell.
EOF
