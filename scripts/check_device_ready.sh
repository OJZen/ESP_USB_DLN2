#!/usr/bin/env bash
set -u

VID_PID="a257:2013"
status=0

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
for master in /sys/class/spi_master/spi*; do
    [ -e "$master" ] || continue
    target="$(readlink -f "$master")"
    if grep -q '^DRIVER=dln2-spi$' "$target/device/uevent" 2>/dev/null; then
        spi_master="${master##*/}"
        ok "SPI master $spi_master is backed by dln2-spi"
        break
    fi
done
[ -n "$spi_master" ] || warn "DLN2 SPI master not found"

spidev_node=""
for dev in /dev/spidev*; do
    [ -e "$dev" ] || continue
    spidev_node="$dev"
    ok "SPI spidev node $dev"
    check_rw "$dev"
    break
done
[ -n "$spidev_node" ] || warn "spidev node not found; run sudo systemctl start esp-usb-dln2-spi.service"

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
