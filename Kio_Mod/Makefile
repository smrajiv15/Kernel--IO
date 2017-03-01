#KERNEL_SOURCE := /root/Desktop/kernel_3_16/linux-3.16-rc3/
obj-m := kernelio.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean
