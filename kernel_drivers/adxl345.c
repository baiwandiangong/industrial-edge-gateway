/*
 * ADXL345 3-axis accelerometer driver
 * Exposes /dev/adxl345 and returns raw XYZ samples as 3 signed short values.
 * Compatible with Linux kernel 4.1.x style drivers.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#define ADXL345_NAME            "adxl345"

/* ADXL345 register map */
#define ADXL345_REG_DEVID       0x00
#define ADXL345_REG_POWER_CTL   0x2D
#define ADXL345_REG_DATA_FORMAT 0x31
#define ADXL345_REG_BW_RATE     0x2C
#define ADXL345_REG_DATAX0      0x32

/* ADXL345 register values */
#define ADXL345_DEVID_VALUE     0xE5
#define ADXL345_MEASURE_MODE    0x08
#define ADXL345_FULL_RES_2G     0x08
#define ADXL345_RATE_100HZ      0x0A

struct adxl345_axis_data {
    short x;
    short y;
    short z;
};

struct adxl345_dev {
    struct spi_device *spi;
    struct miscdevice miscdev;
    struct mutex lock;
};

static int adxl345_read_regs(struct adxl345_dev *adxl345,
                             u8 reg, void *buf, size_t len)
{
    u8 tx;

    /*
     * Bit7: read, Bit6: multi-byte.
     * DATAX0..DATAZ1 are contiguous, so multi-byte burst read is enough.
     */
    tx = reg | 0x80;
    if (len > 1)
        tx |= 0x40;

    return spi_write_then_read(adxl345->spi, &tx, 1, buf, len);
}

static int adxl345_write_reg(struct adxl345_dev *adxl345, u8 reg, u8 value)
{
    u8 tx[2];

    tx[0] = reg & 0x3F;
    tx[1] = value;
    return spi_write(adxl345->spi, tx, sizeof(tx));
}

static int adxl345_hw_init(struct adxl345_dev *adxl345)
{
    int ret;
    u8 devid;

    ret = adxl345_read_regs(adxl345, ADXL345_REG_DEVID, &devid, 1);
    if (ret < 0) {
        dev_err(&adxl345->spi->dev, "failed to read device id: %d\n", ret);
        return ret;
    }

    if (devid != ADXL345_DEVID_VALUE) {
        dev_err(&adxl345->spi->dev, "unexpected device id: 0x%02x\n", devid);
        return -ENODEV;
    }

    ret = adxl345_write_reg(adxl345, ADXL345_REG_POWER_CTL, 0x00);
    if (ret < 0)
        return ret;

    ret = adxl345_write_reg(adxl345, ADXL345_REG_DATA_FORMAT, ADXL345_FULL_RES_2G);
    if (ret < 0)
        return ret;

    ret = adxl345_write_reg(adxl345, ADXL345_REG_BW_RATE, ADXL345_RATE_100HZ);
    if (ret < 0)
        return ret;

    ret = adxl345_write_reg(adxl345, ADXL345_REG_POWER_CTL, ADXL345_MEASURE_MODE);
    if (ret < 0)
        return ret;

    msleep(10);
    return 0;
}

static int adxl345_read_xyz(struct adxl345_dev *adxl345,
                            struct adxl345_axis_data *data)
{
    int ret;
    u8 raw[6];

    ret = adxl345_read_regs(adxl345, ADXL345_REG_DATAX0, raw, sizeof(raw));
    if (ret < 0)
        return ret;

    data->x = (short)((raw[1] << 8) | raw[0]);
    data->y = (short)((raw[3] << 8) | raw[2]);
    data->z = (short)((raw[5] << 8) | raw[4]);
    return 0;
}

static int adxl345_open(struct inode *inode, struct file *file)
{
    struct miscdevice *miscdev = file->private_data;
    struct adxl345_dev *adxl345;

    adxl345 = container_of(miscdev, struct adxl345_dev, miscdev);
    file->private_data = adxl345;
    return 0;
}

static ssize_t adxl345_read(struct file *file, char __user *buf,
                            size_t size, loff_t *ppos)
{
    struct adxl345_dev *adxl345 = file->private_data;
    struct adxl345_axis_data data;
    int ret;

    if (size < sizeof(data))
        return -EINVAL;

    mutex_lock(&adxl345->lock);
    ret = adxl345_read_xyz(adxl345, &data);
    mutex_unlock(&adxl345->lock);
    if (ret < 0)
        return ret;

    if (copy_to_user(buf, &data, sizeof(data)))
        return -EFAULT;

    return sizeof(data);
}

static ssize_t adxl345_write(struct file *file, const char __user *buf,
                             size_t size, loff_t *ppos)
{
    return -EOPNOTSUPP;
}

static int adxl345_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations adxl345_fops = {
    .owner = THIS_MODULE,
    .open = adxl345_open,
    .read = adxl345_read,
    .write = adxl345_write,
    .release = adxl345_release,
};

static int adxl345_probe(struct spi_device *spi)
{
    struct adxl345_dev *adxl345;
    int ret;

    adxl345 = devm_kzalloc(&spi->dev, sizeof(*adxl345), GFP_KERNEL);
    if (adxl345 == NULL)
        return -ENOMEM;

    adxl345->spi = spi;
    mutex_init(&adxl345->lock);

    adxl345->miscdev.minor = MISC_DYNAMIC_MINOR;
    adxl345->miscdev.name = ADXL345_NAME;
    adxl345->miscdev.fops = &adxl345_fops;

    spi_set_drvdata(spi, adxl345);

    ret = adxl345_hw_init(adxl345);
    if (ret < 0) {
        dev_err(&spi->dev, "hardware init failed: %d\n", ret);
        return ret;
    }

    ret = misc_register(&adxl345->miscdev);
    if (ret < 0) {
        dev_err(&spi->dev, "misc_register failed: %d\n", ret);
        return ret;
    }

    dev_info(&spi->dev, "ADXL345 probed successfully\n");
    return 0;
}

static int adxl345_remove(struct spi_device *spi)
{
    struct adxl345_dev *adxl345 = spi_get_drvdata(spi);

    misc_deregister(&adxl345->miscdev);
    return 0;
}

static const struct of_device_id adxl345_of_match[] = {
    { .compatible = "adi,adxl345" },
    { }
};
MODULE_DEVICE_TABLE(of, adxl345_of_match);

static const struct spi_device_id adxl345_id[] = {
    { "adxl345", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, adxl345_id);

static struct spi_driver adxl345_driver = {
    .driver = {
        .name = ADXL345_NAME,
        .owner = THIS_MODULE,
        .of_match_table = adxl345_of_match,
    },
    .probe = adxl345_probe,
    .remove = adxl345_remove,
    .id_table = adxl345_id,
};

static int __init adxl345_driver_init(void)
{
    int ret;

    ret = spi_register_driver(&adxl345_driver);
    if (ret < 0) {
        pr_err("adxl345: spi_register_driver failed: %d\n", ret);
        return ret;
    }

    pr_info("adxl345_driver_init\n");
    return 0;
}

static void __exit adxl345_driver_exit(void)
{
    spi_unregister_driver(&adxl345_driver);
    pr_info("adxl345_driver_exit\n");
}

module_init(adxl345_driver_init);
module_exit(adxl345_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("ADXL345 SPI accelerometer driver");
MODULE_VERSION("1.0");
