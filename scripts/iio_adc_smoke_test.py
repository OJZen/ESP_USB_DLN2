#!/usr/bin/env python3
import argparse
from pathlib import Path

from iio_adc_buffer_test import (
    available_voltage_channels,
    configure_scan,
    find_iio_device,
    read_samples,
    read_text,
    set_buffer_enabled,
    write_text,
)


def parse_channels(text: str) -> list[int]:
    channels = [
        int(item, 0)
        for item in text.replace(",", " ").split()
        if item
    ]
    if not channels:
        raise SystemExit("no ADC channels selected")
    if len(set(channels)) != len(channels):
        raise SystemExit("--channels must not contain duplicates")
    return channels


def read_raw_values(device: Path, channels: list[int], raw_min: int,
                    raw_max: int) -> dict[int, int]:
    values = {}
    for channel in channels:
        raw_path = device / f"in_voltage{channel}_raw"
        value = int(read_text(raw_path), 0)
        if value < raw_min or value > raw_max:
            raise AssertionError(
                f"in_voltage{channel}_raw={value} outside {raw_min}..{raw_max}"
            )
        values[channel] = value
    return values


def verify_buffer(device: Path, dev_node: Path, channels: list[int],
                  frequency: str, length: str, samples: int,
                  timeout_s: float, raw_min: int, raw_max: int) -> None:
    try:
        set_buffer_enabled(device, False)
        configure_scan(device, channels)
        write_text(device / "sampling_frequency", frequency)
        write_text(device / "buffer" / "length", length)
        set_buffer_enabled(device, True)

        frames = read_samples(dev_node, channels, samples, timeout_s)
    finally:
        try:
            set_buffer_enabled(device, False)
        except Exception:
            pass
        try:
            configure_scan(device, [])
        except Exception:
            pass

    if len(frames) != samples:
        raise AssertionError(f"buffer returned {len(frames)} samples, expected {samples}")

    for sample_index, frame in enumerate(frames):
        for channel, value in zip(channels, frame):
            if value < raw_min or value > raw_max:
                raise AssertionError(
                    f"sample {sample_index} in_voltage{channel}={value} "
                    f"outside {raw_min}..{raw_max}"
                )

    for index, frame in enumerate(frames):
        values = " ".join(
            f"in_voltage{channel}={value}"
            for channel, value in zip(channels, frame)
        )
        print(f"buffer[{index}]: {values}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run automatically checkable DLN-2 ADC IIO smoke tests. "
            "This does not validate analog accuracy; known-voltage testing is manual."
        )
    )
    parser.add_argument("--device-name", default="dln2-adc")
    parser.add_argument("--channels", default="0 1 2 3",
                        help="space or comma separated channel numbers")
    parser.add_argument("--frequency", default="10",
                        help="sampling_frequency value to write")
    parser.add_argument("--length", default="8",
                        help="IIO buffer length")
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--raw-min", type=int, default=0)
    parser.add_argument("--raw-max", type=int, default=1023)
    parser.add_argument("--skip-buffer", action="store_true",
                        help="only validate sysfs raw reads")
    args = parser.parse_args()

    if args.samples < 1:
        raise SystemExit("--samples must be positive")
    if args.raw_min > args.raw_max:
        raise SystemExit("--raw-min must be <= --raw-max")

    channels = parse_channels(args.channels)
    device = find_iio_device(args.device_name)
    valid_channels = available_voltage_channels(device)
    invalid = [channel for channel in channels if channel not in valid_channels]
    if invalid:
        raise SystemExit(f"invalid channels {invalid}; available={valid_channels}")

    dev_node = Path("/dev") / device.name
    print(f"device={device} dev={dev_node} channels={channels}")

    if (device / "buffer" / "enable").exists():
        set_buffer_enabled(device, False)

    raw_values = read_raw_values(device, channels, args.raw_min, args.raw_max)
    print(
        "raw: " + " ".join(
            f"in_voltage{channel}_raw={value}"
            for channel, value in raw_values.items()
        )
    )

    if not args.skip_buffer:
        verify_buffer(
            device,
            dev_node,
            channels,
            args.frequency,
            args.length,
            args.samples,
            args.timeout,
            args.raw_min,
            args.raw_max,
        )

    print("PASS ADC IIO smoke test")
    print("INFO known-voltage electrical accuracy test is manual and not part of this check")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PermissionError as exc:
        raise SystemExit(
            f"permission denied: {exc.filename}; run host setup or fix IIO permissions"
        )
