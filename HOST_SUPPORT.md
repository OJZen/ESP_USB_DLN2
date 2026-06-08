# Host Support

The ESP32-S3 firmware enumerates as a DLN-2 compatible USB multifunction
device. Linux automatically creates GPIO, I2C, SPI master, and IIO ADC child
devices, but a generic SPI userspace node needs one host-side step because
Linux does not create `/dev/spidev*` for a bare SPI controller.

Install the host support once:

```bash
sudo ./scripts/install_host_support.sh
```

By default the installer uses group `plugdev` and uses DKMS when `dkms` is
available. DKMS lets the SPI helper rebuild automatically after kernel
upgrades. If DKMS is not installed, the script falls back to a current-kernel
module installed under `/usr/local/lib/esp_usb_dln2/`.

To use another access group:

```bash
sudo ./scripts/install_host_support.sh --group i2c
```

To require DKMS or force the direct current-kernel fallback:

```bash
sudo ./scripts/install_host_support.sh --dkms yes
sudo ./scripts/install_host_support.sh --dkms no
```

After installation, unplug/replug the S3 or run:

```bash
sudo systemctl start esp-usb-dln2-spi.service
esp-usb-dln2-check
```

The install provides:

- `/etc/udev/rules.d/99-zz-esp-usb-dln2.rules`
- `/etc/systemd/system/esp-usb-dln2-spi.service`
- `/etc/esp_usb_dln2/host.conf`
- `/usr/src/esp-usb-dln2-spi-0.2.0/` plus DKMS metadata when DKMS is used
- `/usr/local/lib/esp_usb_dln2/spi_dln2_spidev_test.ko` when DKMS is not used
- `/usr/local/libexec/esp_usb_dln2/esp_usb_dln2_setup_host_device.sh`
- `/usr/local/bin/esp-usb-dln2-check`
- `/usr/local/sbin/esp-usb-dln2-uninstall`

Expected usable devices after plug-in:

- `/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00`
- a `/dev/i2c-X` adapter whose name starts with `dln2-i2c-`
- a `/dev/spidevX.0` node on the DLN-2 SPI bus for lab SPI userspace access
- `/dev/iio:deviceX` whose IIO name is `dln2-adc`

The SPI helper is a host-side registrar for a temporary `spidev` child on the
DLN-2 SPI master. This is intentionally outside the S3 firmware because Linux
models SPI controllers and SPI child devices separately. The firmware can
provide the controller, but Linux still needs a described SPI child before it
creates a `spidev` node.

If the DLN-2 `/dev/spidevX.0` node is missing after replug:

```bash
sudo systemctl start esp-usb-dln2-spi.service
journalctl -u esp-usb-dln2-spi.service -n 50 --no-pager
```

To remove the host support package:

```bash
sudo esp-usb-dln2-uninstall
```

or from the repository checkout:

```bash
sudo ./scripts/uninstall_host_support.sh
```
