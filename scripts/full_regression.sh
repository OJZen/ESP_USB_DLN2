#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

C3_PORT="${C3_PORT:-/dev/ttyACM2}"
SPIDEV="${SPIDEV:-/dev/spidev0.0}"
I2C_ADDR="${I2C_ADDR:-0x52}"
ADC_CHANNELS="${ADC_CHANNELS:-0 1 2 3}"
ADC_FREQUENCY="${ADC_FREQUENCY:-10}"
ADC_SAMPLES="${ADC_SAMPLES:-8}"
LOG_DIR="${LOG_DIR:-${REPO_ROOT}/test_logs}"
LOOPS=1

RUN_HOST_CHECK=1
RUN_FIXED_HARNESS=1
RUN_ADC=1
RUN_I2C_STRESS=0
RUN_SPI_MATRIX=0

usage() {
    cat <<EOF
Usage: $0 [options]

Run the standard ESP USB DLN2 regression for the fixed C3/S3 harness.

Default checks:
  1. host readiness through scripts/check_device_ready.sh
  2. fixed C3 harness GPIO/I2C/SPI smoke through c3_fixed_harness_regression.sh
  3. ADC IIO smoke: raw reads and 4-channel buffered reads

ADC known-voltage electrical accuracy is intentionally not automated here.

Options:
  --c3-port PATH       C3 serial port (default: ${C3_PORT})
  --spidev PATH        spidev node (default: ${SPIDEV})
  --i2c-addr ADDR      C3 I2C slave address (default: ${I2C_ADDR})
  --adc-channels LIST  ADC channels to check (default: "${ADC_CHANNELS}")
  --adc-samples N      ADC buffered sample count (default: ${ADC_SAMPLES})
  --loops N            Repeat host/harness/ADC checks N times (default: ${LOOPS})
  --log-dir PATH       Directory for run logs (default: ${LOG_DIR})
  --with-i2c-stress    Also run short I2C error-recovery/stress coverage
  --with-spi-matrix    Also run SPI modes 0-3 and read/write/full directions
  --extended           Equivalent to --with-i2c-stress --with-spi-matrix
  --skip-host-check    Do not run host readiness diagnostics
  --skip-harness       Do not run C3 fixed-harness GPIO/I2C/SPI tests
  --skip-adc           Do not run ADC IIO smoke
  -h, --help           Show this help

Environment variables:
  C3_PORT, SPIDEV, I2C_ADDR, ADC_CHANNELS, ADC_FREQUENCY, ADC_SAMPLES, LOG_DIR
EOF
}

positive_int() {
    local name="$1"
    local value="$2"
    if [[ ! "$value" =~ ^[0-9]+$ ]] || [ "$value" -lt 1 ]; then
        echo "$name must be a positive integer" >&2
        exit 2
    fi
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
        --adc-channels)
            ADC_CHANNELS="${2:?missing value for --adc-channels}"
            shift 2
            ;;
        --adc-samples)
            ADC_SAMPLES="${2:?missing value for --adc-samples}"
            shift 2
            ;;
        --loops)
            LOOPS="${2:?missing value for --loops}"
            shift 2
            ;;
        --log-dir)
            LOG_DIR="${2:?missing value for --log-dir}"
            shift 2
            ;;
        --with-i2c-stress)
            RUN_I2C_STRESS=1
            shift
            ;;
        --with-spi-matrix)
            RUN_SPI_MATRIX=1
            shift
            ;;
        --extended)
            RUN_I2C_STRESS=1
            RUN_SPI_MATRIX=1
            shift
            ;;
        --skip-host-check)
            RUN_HOST_CHECK=0
            shift
            ;;
        --skip-harness)
            RUN_FIXED_HARNESS=0
            shift
            ;;
        --skip-adc)
            RUN_ADC=0
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

positive_int "--loops" "$LOOPS"
positive_int "--adc-samples" "$ADC_SAMPLES"

if ! ((RUN_HOST_CHECK || RUN_FIXED_HARNESS || RUN_ADC)); then
    echo "nothing to run: all checks are disabled" >&2
    exit 2
fi

mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/full_regression_$(date +%Y%m%d-%H%M%S).log"

exec > >(tee -a "$LOG_FILE") 2>&1

finish() {
    local rc="$?"
    if [ "$rc" -eq 0 ]; then
        printf '\nPASS full regression log=%s\n' "$LOG_FILE"
    else
        printf '\nFAIL full regression rc=%s log=%s\n' "$rc" "$LOG_FILE"
    fi
}
trap finish EXIT

run() {
    printf '\n==> %s\n' "$*"
    "$@"
}

printf 'ESP USB DLN2 full regression\n'
printf 'time=%s\n' "$(date --iso-8601=seconds)"
printf 'loops=%s c3=%s spidev=%s i2c_addr=%s adc_channels="%s"\n' \
    "$LOOPS" "$C3_PORT" "$SPIDEV" "$I2C_ADDR" "$ADC_CHANNELS"

for ((loop = 1; loop <= LOOPS; loop++)); do
    printf '\n===== loop %d/%d =====\n' "$loop" "$LOOPS"

    if ((RUN_HOST_CHECK)); then
        run "${SCRIPT_DIR}/check_device_ready.sh"
    fi

    if ((RUN_FIXED_HARNESS)); then
        harness_args=(
            --c3-port "$C3_PORT"
            --spidev "$SPIDEV"
            --i2c-addr "$I2C_ADDR"
        )
        if ((RUN_I2C_STRESS)); then
            harness_args+=(--with-i2c-stress)
        fi
        if ((RUN_SPI_MATRIX)); then
            harness_args+=(--with-spi-matrix)
        fi
        run "${SCRIPT_DIR}/c3_fixed_harness_regression.sh" "${harness_args[@]}"
    fi

    if ((RUN_ADC)); then
        run "${SCRIPT_DIR}/iio_adc_smoke_test.py" \
            --channels "$ADC_CHANNELS" \
            --frequency "$ADC_FREQUENCY" \
            --samples "$ADC_SAMPLES"
    fi
done
