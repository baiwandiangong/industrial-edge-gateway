/*
 * SHT30 humidity and temperature sensor driver
 * Using miscdevice for /dev/sht30 access
 * Compatible with Linux kernel 4.1.x
 */

#include <linux/init.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/export.h>
#include <linux/types.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/string.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#define DEV_NAME "sht30"

/* SHT30 commands */
#define SHT30_CMD_MEASURE_HIGH_REP 0x2C06
#define SHT30_CMD_MEASURE_MED_REP  0x2C0D
#define SHT30_CMD_MEASURE_LOW_REP  0x2C10
#define SHT30_CMD_SOFT_RESET       0x30A2

/* SHT30 I2C address */
#define SHT30_I2C_ADDR 0x44

static struct i2c_client *sht30_client;

/* SHT30 data structure */
struct sht30_data {
    short temperature;  /* Temperature in 0.01 degrees Celsius */
    short humidity;     /* Humidity in 0.01 percent */
};

static int sht30_open(struct inode *inode, struct file *file)
{
    printk("sht30 open\n");
    return 0;
}

static int sht30_read_sensor(struct sht30_data *data)
{
    int ret;
    u8 cmd[2];
    u8 buf[6];
    struct i2c_msg msgs[2];
    u16 temp_raw, hum_raw;

    if (!sht30_client)
        return -ENODEV;

    /* Send measurement command */
    cmd[0] = (SHT30_CMD_MEASURE_HIGH_REP >> 8) & 0xFF;
    cmd[1] = SHT30_CMD_MEASURE_HIGH_REP & 0xFF;

    msgs[0].addr = sht30_client->addr;
    msgs[0].flags = 0;
    msgs[0].len = 2;
    msgs[0].buf = cmd;

    ret = i2c_transfer(sht30_client->adapter, msgs, 1);
    if (ret < 0) {
        printk("sht30: Failed to send command: %d\n", ret);
        return ret;
    }

    /* Wait for measurement (high repeatability takes ~15ms) */
    msleep(20);

    /* Read measurement data (6 bytes: temp + CRC + hum + CRC) */
    msgs[0].addr = sht30_client->addr;
    msgs[0].flags = I2C_M_RD;
    msgs[0].len = 6;
    msgs[0].buf = buf;

    ret = i2c_transfer(sht30_client->adapter, msgs, 1);
    if (ret < 0) {
        printk("sht30: Failed to read data: %d\n", ret);
        return ret;
    }

    /* Parse temperature and humidity */
    temp_raw = (buf[0] << 8) | buf[1];
    hum_raw = (buf[3] << 8) | buf[4];

    /* Convert to 0.01 degrees Celsius */
    data->temperature = ((17500 * temp_raw) / 65535) - 4500;

    /* Convert to 0.01 percent relative humidity */
    data->humidity = (10000 * hum_raw) / 65535;

    return 0;
}

static ssize_t sht30_read(struct file *file, char __user *buf, size_t size, loff_t *loff)
{
    int ret;
    struct sht30_data data;

    ret = sht30_read_sensor(&data);
    if (ret < 0)
        return ret;

    ret = copy_to_user(buf, &data, sizeof(data));
    if (ret)
        return -EFAULT;

    return sizeof(data);
}

static ssize_t sht30_write(struct file *file, const char __user *buf, size_t size, loff_t *loff)
{
    return 0;
}

static int sht30_close(struct inode *inode, struct file *file)
{
    printk("sht30 close\n");
    return 0;
}

static struct file_operations sht30_fops = {
    .owner = THIS_MODULE,
    .open = sht30_open,
    .read = sht30_read,
    .write = sht30_write,
    .release = sht30_close
};

static struct miscdevice sht30_misc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEV_NAME,
    .fops = &sht30_fops
};

static int sht30_probe(struct i2c_client *client, const struct i2c_device_id *device)
{
    int ret;
    u8 cmd[2];
    struct i2c_msg msg;

    ret = misc_register(&sht30_misc_dev);
    if (ret < 0)
        goto err_misc_register;

    sht30_client = client;

    /* Reset sensor */
    cmd[0] = (SHT30_CMD_SOFT_RESET >> 8) & 0xFF;
    cmd[1] = SHT30_CMD_SOFT_RESET & 0xFF;

    msg.addr = client->addr;
    msg.flags = 0;
    msg.len = 2;
    msg.buf = cmd;

    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret < 0) {
        printk("sht30: Failed to reset sensor: %d\n", ret);
    }

    msleep(10);

    printk("sht30 probe success, addr = 0x%02x\n", client->addr);
    return 0;

err_misc_register:
    printk("sht30 probe misc_register failed\n");
    return ret;
}

static int sht30_remove(struct i2c_client *client)
{
    misc_deregister(&sht30_misc_dev);
    sht30_client = NULL;
    printk("sht30 remove\n");
    return 0;
}

static const struct i2c_device_id sht30_table[] = {
    {"sht30", 0},
    {"sensirion,sht30", 0},
    {}
};

static const struct of_device_id of_sht30_table[] = {
    {.compatible = "sensirion,sht30"},
    {}
};

static struct i2c_driver sht30_driver = {
    .probe = sht30_probe,
    .remove = sht30_remove,
    .driver = {
        .name = DEV_NAME,
        .of_match_table = of_sht30_table
    },
    .id_table = sht30_table
};

static int __init sht30_driver_init(void)
{
    int ret = i2c_add_driver(&sht30_driver);
    if (ret < 0)
        goto err_i2c_add;
    printk("sht30_driver_init ...\n");
    return 0;

err_i2c_add:
    printk("sht30_driver_init failed...\n");
    return ret;
}

static void __exit sht30_driver_exit(void)
{
    i2c_del_driver(&sht30_driver);
    printk("sht30_driver_exit ...\n");
}

module_init(sht30_driver_init);
module_exit(sht30_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("SHT30 humidity and temperature sensor driver");
MODULE_VERSION("1.0");
