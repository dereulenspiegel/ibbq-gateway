#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := iBBQGateway

include $(IDF_PATH)/make/project.mk

flash_spiffs:
	mkspiffs -c ./data -b 4096 -p 256 -s 0x2F000 spiffs.bin
	python2 $(IDF_PATH)/components/esptool_py/esptool/esptool.py --chip esp32 --port /dev/cu.SLAB_USBtoUART --baud 115200 write_flash -z 0x3D1000 spiffs.bin

debug_gdb:
	xtensa-esp32-elf-gdb ./build/iBBQGateway.elf -b 115200 -ex 'target remote /dev/cu.SLAB_USBtoUART'
