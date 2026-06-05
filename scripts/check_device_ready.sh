#!/usr/bin/env bash
set -u

VID_PID="a257:2013"
CONFIG_FILE="/etc/esp_usb_dln2/host.conf"
SPI_CS="${ESP_USB_DLN2_SPI_CS:-0}"
status=0

if [ -r "$CONFIG_FILE" ]; then
    # shellcheck disable=SC1090
    . "$CONFIG_FILE"
    SPI_CS="${ESP_USB_DLN2_SPI_CS:-$SPI_CS}"
fi

ok() {
    printf 'OK   %s\n' "$*"
}

warn() {
    printf 'WARN %s\n' "$*"
    status=1
}

check_rw() {
    local path="$1"
    if [ -r "$path" ] && [ -w "$path" ]; then
        ok "$path is readable/writable"
    else
        warn "$path exists but is not readable/writable by $(id -un)"
    fi
}

if lsusb -d "$VID_PID" >/dev/null 2>&1; then
    ok "USB device $VID_PID is present"
else
    warn "USB device $VID_PID is not present"
fi

gpio_link=""
for candidate in /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_*-if00; do
    [ -e "$candidate" ] || continue
    gpio_link="$candidate"
    break
done

if [ -n "$gpio_link" ]; then
    gpio_node="$(readlink -f "$gpio_link")"
    ok "GPIO chip $gpio_link -> $gpio_node"
    check_rw "$gpio_node"
else
    warn "GPIO by-id link not found"
fi

i2c_node=""
for dev in /sys/class/i2c-dev/i2c-*; do
    [ -e "$dev" ] || continue
    name="$(cat "$dev/name" 2>/dev/null || true)"
    case "$name" in
        dln2-i2c-*)
            i2c_node="/dev/${dev##*/}"
            ok "I2C adapter $i2c_node name=$name"
            check_rw "$i2c_node"
            break
            ;;
    esac
done
[ -n "$i2c_node" ] || warn "DLN2 I2C adapter not found"

spi_master=""
spi_bus=""
for master in /sys/class/spi_master/spi*; do
    [ -e "$master" ] || continue
    target="$(readlink -f "$master")"
    if grep -q '^DRIVER=dln2-spi$' "$target/device/uevent" 2>/dev/null; then
        spi_master="${master##*/}"
        spi_bus="${spi_master#spi}"
        ok "SPI master $spi_master is backed by dln2-spi"
        break
    fi
done
[ -n "$spi_master" ] || warn "DLN2 SPI master not found"

spidev_node=""
if [ -n "$spi_bus" ]; then
    candidate="/dev/spidev${spi_bus}.${SPI_CS}"
    if [ -e "$candidate" ]; then
        spidev_node="$candidate"
        ok "SPI spidev node $spidev_node"
        check_rw "$spidev_node"
    else
        warn "$candidate not found; run sudo systemctl start esp-usb-dln2-spi.service"
    fi
else
    warn "spidev node not checked because DLN2 SPI master is missing"
fi

iio_node=""
for dev in /sys/bus/iio/devices/iio:device*; do
    [ -e "$dev" ] || continue
    name="$(cat "$dev/name" 2>/dev/null || true)"
    if [ "$name" = "dln2-adc" ]; then
        iio_node="/dev/${dev##*/}"
        ok "IIO ADC $iio_node name=$name"
        check_rw "$iio_node"
        if [ -w "$dev/buffer/enable" ] || [ -w "$dev/buffer0/enable" ]; then
            ok "IIO buffer sysfs is writable"
        else
            warn "IIO buffer sysfs is not writable by $(id -un)"
        fi
        break
    fi
done
[ -n "$iio_node" ] || warn "DLN2 IIO ADC not found"

exit "$status"
