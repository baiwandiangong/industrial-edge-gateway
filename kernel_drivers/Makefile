ifneq ($(KERNELRELEASE),)
obj-m += sht30.o
obj-m += adxl345.o
else
KDIR ?= /path/to/kernel
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
