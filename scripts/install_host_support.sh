#!/usr/bin/env bash
set -euo pipefail

ACCESS_GROUP="${ESP_USB_DLN2_ACCESS_GROUP:-plugdev}"
DKMS_MODE="${ESP_USB_DLN2_DKMS:-auto}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_RELEASE="$(uname -r)"
SPI_TOOL_DIR="$REPO_ROOT/tools/spi_test_device"
INSTALL_LIB_DIR="/usr/local/lib/esp_usb_dln2"
INSTALL_LIBEXEC_DIR="/usr/local/libexec/esp_usb_dln2"
INSTALL_SBIN_DIR="/usr/local/sbin"
CONFIG_DIR="/etc/esp_usb_dln2"
UDEV_RULE="/etc/udev/rules.d/99-zz-esp-usb-dln2.rules"
OLD_UDEV_RULE="/etc/udev/rules.d/99-esp-usb-dln2.rules"
SYSTEMD_UNIT="/etc/systemd/system/esp-usb-dln2-spi.service"
DKMS_PACKAGE="esp-usb-dln2-spi"
DKMS_VERSION="0.2.0"
DKMS_SOURCE_DIR="/usr/src/${DKMS_PACKAGE}-${DKMS_VERSION}"
SPI_MODULE_NAME="spi_dln2_spidev_test"
SPI_MODULE_PATH="$INSTALL_LIB_DIR/${SPI_MODULE_NAME}.ko"
SPI_MODULE_VALUE=""
BUILD_DIR=""

cleanup() {
    if [ -n "$BUILD_DIR" ] && [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
}
trap cleanup EXIT

usage() {
    cat <<EOF
Usage: sudo $0 [--group GROUP] [--dkms auto|yes|no]

Installs host support for the ESP USB DLN2 device:
  - udev permissions for GPIO/I2C/IIO/spidev
  - systemd oneshot service triggered when the USB device is plugged in
  - SPI spidev child registrar module, preferably managed by DKMS
  - esp-usb-dln2-check readiness command
EOF
}

require_command() {
    local cmd="$1"
    local message="$2"

    command -v "$cmd" >/dev/null 2>&1 || {
        printf '%s\n' "$message" >&2
        exit 1
    }
}

choose_dkms_mode() {
    case "$DKMS_MODE" in
        auto)
            if command -v dkms >/dev/null 2>&1; then
                DKMS_MODE=yes
            else
                DKMS_MODE=no
                printf 'DKMS is not installed; falling back to current-kernel module install.\n'
            fi
            ;;
        yes|no)
            ;;
        *)
            printf 'Invalid --dkms value: %s\n' "$DKMS_MODE" >&2
            usage >&2
            exit 2
            ;;
    esac
}

install_spi_helper_dkms() {
    printf 'Installing SPI helper through DKMS package %s/%s...\n' \
        "$DKMS_PACKAGE" "$DKMS_VERSION"

    require_command dkms "dkms is required when --dkms yes is used."

    dkms remove -m "$DKMS_PACKAGE" -v "$DKMS_VERSION" --all >/dev/null 2>&1 || true
    rm -rf "$DKMS_SOURCE_DIR"
    install -d "$DKMS_SOURCE_DIR"
    install -m 0644 "$SPI_TOOL_DIR/Makefile" "$DKMS_SOURCE_DIR/Makefile"
    install -m 0644 "$SPI_TOOL_DIR/spi_dln2_spidev_test.c" \
        "$DKMS_SOURCE_DIR/spi_dln2_spidev_test.c"
    sed \
        -e "s/@DKMS_PACKAGE@/$DKMS_PACKAGE/g" \
        -e "s/@DKMS_VERSION@/$DKMS_VERSION/g" \
        "$SPI_TOOL_DIR/dkms.conf.in" > "$DKMS_SOURCE_DIR/dkms.conf"
    chmod 0644 "$DKMS_SOURCE_DIR/dkms.conf"

    dkms add -m "$DKMS_PACKAGE" -v "$DKMS_VERSION"
    dkms build -m "$DKMS_PACKAGE" -v "$DKMS_VERSION" -k "$KERNEL_RELEASE"
    dkms install -m "$DKMS_PACKAGE" -v "$DKMS_VERSION" -k "$KERNEL_RELEASE"
    depmod "$KERNEL_RELEASE"

    SPI_MODULE_VALUE="$SPI_MODULE_NAME"
}

install_spi_helper_direct() {
    printf 'Building SPI helper for kernel %s...\n' "$KERNEL_RELEASE"
    BUILD_DIR="$(mktemp -d /tmp/esp_usb_dln2_spi_helper.XXXXXX)"
    install -m 0644 "$SPI_TOOL_DIR/Makefile" "$BUILD_DIR/Makefile"
    install -m 0644 "$SPI_TOOL_DIR/spi_dln2_spidev_test.c" \
        "$BUILD_DIR/spi_dln2_spidev_test.c"
    make -C "$BUILD_DIR" all

    install -Dm644 "$BUILD_DIR/${SPI_MODULE_NAME}.ko" "$SPI_MODULE_PATH"

    SPI_MODULE_VALUE="$SPI_MODULE_PATH"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --group)
            ACCESS_GROUP="${2:-}"
            [ -n "$ACCESS_GROUP" ] || { usage >&2; exit 2; }
            shift 2
            ;;
        --dkms)
            DKMS_MODE="${2:-}"
            [ -n "$DKMS_MODE" ] || { usage >&2; exit 2; }
            shift 2
            ;;
        --no-dkms)
            DKMS_MODE=no
            shift
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

choose_dkms_mode

if [ ! -d "/lib/modules/$KERNEL_RELEASE/build" ]; then
    printf 'Missing kernel build directory: /lib/modules/%s/build\n' "$KERNEL_RELEASE" >&2
    printf 'Install kernel headers for the running kernel, then rerun this script.\n' >&2
    exit 1
fi

require_command make "make is required to build the SPI helper module."
require_command systemctl "systemctl is required for automatic plug-in setup."
require_command udevadm "udevadm is required for device permission rules."
require_command depmod "depmod is required to refresh kernel module metadata."

if [ "$DKMS_MODE" = "yes" ]; then
    install_spi_helper_dkms
else
    install_spi_helper_direct
fi

install -Dm755 "$REPO_ROOT/scripts/esp_usb_dln2_setup_host_device.sh" \
    "$INSTALL_LIBEXEC_DIR/esp_usb_dln2_setup_host_device.sh"
install -Dm755 "$REPO_ROOT/scripts/check_device_ready.sh" \
    /usr/local/bin/esp-usb-dln2-check
install -Dm755 "$REPO_ROOT/scripts/uninstall_host_support.sh" \
    "$INSTALL_SBIN_DIR/esp-usb-dln2-uninstall"

install -d "$CONFIG_DIR"
sed \
    -e "s/@ACCESS_GROUP@/$ACCESS_GROUP/g" \
    -e "s|@SPI_MODULE@|$SPI_MODULE_VALUE|g" \
    "$REPO_ROOT/host_support/host.conf.in" \
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
DKMS mode:    $DKMS_MODE
SPI helper:   $SPI_MODULE_VALUE
Config:       $CONFIG_DIR/host.conf
udev rule:    $UDEV_RULE
systemd unit: $SYSTEMD_UNIT
Uninstall:    sudo esp-usb-dln2-uninstall

Run:
  esp-usb-dln2-check

If your user is not in group '$ACCESS_GROUP', add it and start a new login shell.
EOF
