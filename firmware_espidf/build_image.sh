#!/bin/bash
BOARD_TYPE="$1"

if [ -z "${BOARD_TYPE}" ]; then
  echo "Syntax: $0 <board type>"
  exit 1
fi


# No LittleFS image: the web UI is in firmware.bin, and a blank spiffs partition is formatted on first boot.
echo "Building image from existing .bin-files"
if [ "${BOARD_TYPE}" == "custom" ]; then
  # Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: the P4 boots its bootloader from 0x2000.
  esptool.py --chip esp32p4 merge_bin -o merged-flash.bin --flash_mode qio --flash_size 32MB 0x2000 .pio/build/custom_pcb/bootloader.bin 0x8000 .pio/build/custom_pcb/partitions.bin 0x10000 .pio/build/custom_pcb/firmware.bin
elif [ "${BOARD_TYPE}" == "sonoff" ]; then
  esptool.py --chip esp32 merge_bin -o merged-flash.bin --flash_mode dio --flash_size 4MB 0x1000 .pio/build/original_sonoff/bootloader.bin 0x8000 .pio/build/original_sonoff/partitions.bin 0x10000 .pio/build/original_sonoff/firmware.bin
else
  echo "Unknown board type ${BOARD_TYPE}! Will error exit!"
  exit 2
fi