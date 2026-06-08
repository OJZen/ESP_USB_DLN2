#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

C3_PORT="${C3_PORT:-/dev/ttyACM2}"
SPIDEV="${SPIDEV:-/dev/spidev0.0}"
I2C_ADDR="${I2C_ADDR:-0x52}"

RUN_GPIO=1
RUN_I2C=1
RUN_SPI=1
RUN_I2C_STRESS=0
RUN_SPI_MATRIX=0

usage() {
    cat <<EOF
Usage: $0 [options]

Run regression tests for the fixed ESP32-C3/ESP32-S3 no-rewire harness v1.

Options:
  --c3-port PATH       C3 serial port (default: ${C3_PORT})
  --spidev PATH        spidev node (default: ${SPIDEV})
  --i2c-addr ADDR      C3 I2C slave address (default: ${I2C_ADDR})
  --gpio-only          Run only fixed-harness GPIO smoke/IRQ test
  --i2c-only           Run only I2C tests
  --spi-only           Run only SPI tests
  --with-i2c-stress    Also run short I2C stress/error-recovery test
  --with-spi-matrix    Also run SPI modes 0-3 and read/write/full directions
  -h, --help           Show this help

Environment variables:
  C3_PORT, SPIDEV, I2C_ADDR
EOF
}

select_only() {
    RUN_GPIO=0
    RUN_I2C=0
    RUN_SPI=0
}

while (($#)); do
    case "$1" in
        --c3-port)
            C3_PORT="${2:?missing value for --c3-port}"
            shift 2
            ;;
        --spidev)
            SPIDEV="${2:?missing value for --spidev}"
            shift 2
            ;;
        --i2c-addr)
            I2C_ADDR="${2:?missing value for --i2c-addr}"
            shift 2
            ;;
        --gpio-only)
            select_only
            RUN_GPIO=1
            shift
            ;;
        --i2c-only)
            select_only
            RUN_I2C=1
            shift
            ;;
        --spi-only)
            select_only
            RUN_SPI=1
            shift
            ;;
        --with-i2c-stress)
            RUN_I2C_STRESS=1
            shift
            ;;
        --with-spi-matrix)
            RUN_SPI_MATRIX=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

run() {
    printf '\n==> %s\n' "$*"
    "$@"
}

if ((RUN_GPIO)); then
    run "${SCRIPT_DIR}/c3_closed_loop_test.py" \
        --c3-port "${C3_PORT}" \
        --lines "0 1" \
        --channels "2 7"
fi

if ((RUN_I2C)); then
    run "${SCRIPT_DIR}/i2c_c3_slave_test.py" \
        --c3 "${C3_PORT}" \
        --addr "${I2C_ADDR}" \
        --lengths "1 16 64"

    if ((RUN_I2C_STRESS)); then
        run "${SCRIPT_DIR}/i2c_c3_stress_test.py" \
            --c3 "${C3_PORT}" \
            --addr "${I2C_ADDR}" \
            --bad-addr 0x53 \
            --cycles 50
    fi
fi

if ((RUN_SPI)); then
    run "${SCRIPT_DIR}/spi_c3_slave_test.py" \
        --c3 "${C3_PORT}" \
        --spidev "${SPIDEV}" \
        --mode 0 \
        --speed 100000 \
        --length 16 \
        --direction full

    if ((RUN_SPI_MATRIX)); then
        for mode in 0 1 2 3; do
            for direction in full read write; do
                run "${SCRIPT_DIR}/spi_c3_slave_test.py" \
                    --c3 "${C3_PORT}" \
                    --spidev "${SPIDEV}" \
                    --mode "${mode}" \
                    --speed 100000 \
                    --length 16 \
                    --direction "${direction}"
            done
        done
    fi
fi

printf '\nPASS fixed harness regression\n'
