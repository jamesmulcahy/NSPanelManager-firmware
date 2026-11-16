#!/bin/bash
BOARD_TYPE="$1"

if [ -z "${BOARD_TYPE}" ]; then
  echo "Syntax: $0 <board type>"
  exit 1
fi


function get_partition_offset {
  partition="$1"
  partition_offset=$(grep -Eve "^#" partitions.csv | grep "$partition," | awk '{ print $4 }' | cut -d ',' -f 1)
  echo $partition_offset
}

echo "Building image from existing .bin-files"
if [ "${BOARD_TYPE}" == "custom" ]; then
  esptool.py --chip esp32s3 merge_bin -o merged-flash.bin --flash_mode qio --flash_size 16MB 0x0 .pio/build/custom_pcb/bootloader.bin 0x8000 .pio/build/custom_pcb/partitions.bin 0x10000 .pio/build/custom_pcb/firmware.bin $(get_partition_offset spiffs) .pio/build/custom_pcb/littlefs.bin
elif [ "${BOARD_TYPE}" == "sonoff" ]; then
  esptool.py --chip esp32 merge_bin -o merged-flash.bin --flash_mode dio --flash_size 4MB 0x1000 .pio/build/original_sonoff/bootloader.bin 0x8000 .pio/build/original_sonoff/partitions.bin 0x10000 .pio/build/original_sonoff/firmware.bin $(get_partition_offset spiffs) .pio/build/original_sonoff/littlefs.bin
else
  echo "Unknown board type ${BOARD_TYPE}! Will error exit!"
  exit 2
fi