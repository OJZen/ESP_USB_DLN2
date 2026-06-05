#!/usr/bin/env bash
set -euo pipefail

ACCESS_GROUP="${ESP_USB_DLN2_ACCESS_GROUP:-plugdev}"
DEV_MODE="${ESP_USB_DLN2_DEV_MODE:-0660}"
SPI_BUS="${ESP_USB_DLN2_SPI_BUS:-auto}"
SPI_CS="${ESP_USB_DLN2_SPI_CS:-0}"
SPI_MAX_SPEED_HZ="${ESP_USB_DLN2_SPI_MAX_SPEED_HZ:-100000}"
SPI_MODE="${ESP_USB_DLN2_SPI_MODE:-0}"
SPI_MODALIAS="${ESP_USB_DLN2_SPI_MODALIAS:-dh2228fv}"
SPI_MODULE="${ESP_USB_DLN2_SPI_MODULE:-/usr/local/lib/esp_usb_dln2/spi_dln2_spidev_test.ko}"

log() {
    printf 'esp_usb_dln2_setup: %s\n' "$*"
}

have_group() {
    getent group "$1" >/dev/null 2>&1
}

normalize_group() {
    if have_group "$ACCESS_GROUP"; then
        return
    fi

    log "group '$ACCESS_GROUP' does not exist; falling back to root"
    ACCESS_GROUP=root
}

set_node_perms() {
    local node="$1"

    [ -e "$node" ] || return 0
    chgrp "$ACCESS_GROUP" "$node" 2>/dev/null || true
    chmod "$DEV_MODE" "$node" 2>/dev/null || true
}

set_path_group_rw() {
    local path="$1"

    [ -e "$path" ] || return 0
    chgrp "$ACCESS_GROUP" "$path" 2>/dev/null || true
    chmod g+rw "$path" 2>/dev/null || true
}

find_dln2_spi_bus() {
    local master target bus

    if [ "$SPI_BUS" != "auto" ]; then
        printf '%s\n' "$SPI_BUS"
        return 0
    fi

    for master in /sys/class/spi_master/spi*; do
        [ -e "$master" ] || continue
        target="$(readlink -f "$master")"
        if grep -q '^DRIVER=dln2-spi$' "$target/device/uevent" 2>/dev/null; then
            bus="${master##*/spi}"
            printf '%s\n' "$bus"
            return 0
        fi
    done

    return 1
}

wait_for_dln2_spi_bus() {
    local _ bus

    for _ in $(seq 1 80); do
        if bus="$(find_dln2_spi_bus)"; then
            printf '%s\n' "$bus"
            return 0
        fi
        sleep 0.1
    done

    return 1
}

wait_for_node() {
    local node="$1"
    local _

    for _ in $(seq 1 40); do
        [ -e "$node" ] && return 0
        sleep 0.1
    done

    return 1
}

setup_gpio_perms() {
    local link node

    for link in /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_*-if00; do
        [ -e "$link" ] || continue
        node="$(readlink -f "$link")"
        set_node_perms "$node"
    done
}

setup_i2c_perms() {
    local dev name node

    for dev in /sys/class/i2c-dev/i2c-*; do
        [ -e "$dev" ] || continue
        name="$(cat "$dev/name" 2>/dev/null || true)"
        case "$name" in
            dln2-i2c-*)
                node="/dev/${dev##*/}"
                set_node_perms "$node"
                ;;
        esac
    done
}

setup_iio_perms() {
    local dev name node path

    for dev in /sys/bus/iio/devices/iio:device*; do
        [ -e "$dev" ] || continue
        name="$(cat "$dev/name" 2>/dev/null || true)"
        [ "$name" = "dln2-adc" ] || continue

        node="/dev/${dev##*/}"
        set_node_perms "$node"

        while IFS= read -r path; do
            set_path_group_rw "$path"
        done < <(
            find -H "$dev" -maxdepth 3 -type f \( \
                -name 'in_voltage*_raw' -o \
                -name 'sampling_frequency' -o \
                -name 'current_trigger' -o \
                -name '*_en' -o \
                -name 'enable' -o \
                -name 'length' -o \
                -name 'watermark' \
            \) 2>/dev/null
        )
    done
}

setup_spidev_perms() {
    local bus dev

    if ! bus="$(find_dln2_spi_bus)"; then
        return 0
    fi

    dev="/dev/spidev${bus}.${SPI_CS}"
    set_node_perms "$dev"
}

setup_current_perms() {
    setup_gpio_perms
    setup_i2c_perms
    setup_iio_perms
    setup_spidev_perms
}

settle_current_perms() {
    local _

    for _ in $(seq 1 20); do
        setup_current_perms
        sleep 0.1
    done
}

setup_spi_child() {
    local bus dev spi_sys

    if ! bus="$(wait_for_dln2_spi_bus)"; then
        log "no dln2-spi master found yet"
        return 0
    fi

    dev="/dev/spidev${bus}.${SPI_CS}"
    spi_sys="/sys/bus/spi/devices/spi${bus}.${SPI_CS}"

    if [ -e "$dev" ]; then
        set_node_perms "$dev"
        log "ready $dev"
        return 0
    fi

    if lsmod | awk '{print $1}' | grep -qx 'spi_dln2_spidev_test'; then
        if [ ! -e "$spi_sys" ] || [ ! -e "$dev" ]; then
            log "reloading stale spi_dln2_spidev_test module"
            rmmod spi_dln2_spidev_test || {
                log "cannot unload spi_dln2_spidev_test; is /dev/spidev in use?"
                return 1
            }
        fi
    fi

    modprobe spidev

    if [ ! -r "$SPI_MODULE" ]; then
        log "missing SPI helper module: $SPI_MODULE"
        return 1
    fi

    if [ ! -e "$dev" ]; then
        insmod "$SPI_MODULE" \
            bus_num="$bus" \
            chip_select="$SPI_CS" \
            max_speed_hz="$SPI_MAX_SPEED_HZ" \
            mode="$SPI_MODE" \
            modalias="$SPI_MODALIAS"
    fi

    wait_for_node "$dev" || {
        log "spidev node did not appear: $dev"
        return 1
    }

    set_node_perms "$dev"
    log "ready $dev"
}

main() {
    if [ "$(id -u)" -ne 0 ]; then
        printf 'This setup script must run as root.\n' >&2
        return 1
    fi

    normalize_group
    setup_current_perms
    setup_spi_child
    settle_current_perms
}

main "$@"
