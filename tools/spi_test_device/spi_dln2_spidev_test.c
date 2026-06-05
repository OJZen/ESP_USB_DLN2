// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

static int bus_num;
module_param(bus_num, int, 0444);
MODULE_PARM_DESC(bus_num, "SPI bus number to attach to");

static int chip_select;
module_param(chip_select, int, 0444);
MODULE_PARM_DESC(chip_select, "SPI chip select number");

static int max_speed_hz = 100000;
module_param(max_speed_hz, int, 0444);
MODULE_PARM_DESC(max_speed_hz, "Maximum SPI clock in Hz");

static int mode;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "SPI mode");

static char modalias[SPI_NAME_SIZE] = "dh2228fv";
module_param_string(modalias, modalias, sizeof(modalias), 0444);
MODULE_PARM_DESC(modalias, "SPI device modalias");

static struct spi_device *test_spi;

static int match_dln2_spi_controller(struct device *dev, const void *data)
{
	const int *target_bus = data;
	struct spi_controller *ctlr;

	if (!dev->driver || strcmp(dev->driver->name, "dln2-spi"))
		return 0;

	ctlr = dev_get_drvdata(dev);
	if (!ctlr)
		return 0;

	return ctlr->bus_num == *target_bus;
}

static int __init spi_dln2_spidev_test_init(void)
{
	struct spi_board_info info = { };
	struct spi_controller *ctlr;
	struct spi_device *spi;
	struct device *dev;

	dev = bus_find_device(&platform_bus_type, NULL, &bus_num,
			      match_dln2_spi_controller);
	if (!dev)
		return -ENODEV;

	ctlr = spi_controller_get(dev_get_drvdata(dev));
	put_device(dev);
	if (!ctlr)
		return -ENODEV;

	strscpy(info.modalias, modalias, sizeof(info.modalias));
	info.max_speed_hz = max_speed_hz;
	info.bus_num = bus_num;
	info.chip_select = chip_select;
	info.mode = mode;

	spi = spi_new_device(ctlr, &info);
	spi_controller_put(ctlr);
	if (!spi)
		return -ENODEV;

	test_spi = spi_dev_get(spi);
	if (!test_spi) {
		spi_unregister_device(spi);
		return -ENODEV;
	}

	pr_info("spi_dln2_spidev_test: registered %s on spi%d.%d at %d Hz mode %d\n",
		modalias, bus_num, chip_select, max_speed_hz, mode);
	return 0;
}

static void __exit spi_dln2_spidev_test_exit(void)
{
	if (!test_spi)
		return;

	if (device_is_registered(&test_spi->dev)) {
		pr_info("spi_dln2_spidev_test: unregister spi%d.%d\n",
			bus_num, chip_select);
		spi_unregister_device(test_spi);
	}

	spi_dev_put(test_spi);
	test_spi = NULL;
}

module_init(spi_dln2_spidev_test_init);
module_exit(spi_dln2_spidev_test_exit);

MODULE_DESCRIPTION("Temporary spidev child registrar for DLN2 SPI testing");
MODULE_AUTHOR("esp_usb_dln2 test tooling");
MODULE_LICENSE("GPL");
