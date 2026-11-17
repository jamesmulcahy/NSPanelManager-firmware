#!/bin/bash
# This script will build the firmware .bin file (the setup web UI is part of it)
# It will also upload it to the NSPanelManager

NSPanelManager_address="127.0.0.1"
NSPanelManager_port="8000"

function update_panel_version() {
  # Update version file.
  echo "Updating version number"
  current_version="$(grep -oE "[0-9\.]+" lib/NSPM_Version/NSPM_version.hpp)"
  current_major_version="$(echo $current_version | cut -d . -f 1,2)"
  current_minor_version="$(echo $current_version | cut -d . -f 3)"
  current_minor_version="$((current_minor_version + 1))"
  echo "#define NSPM_VERSION \"$current_major_version.$current_minor_version\"" >lib/NSPM_Version/NSPM_version.hpp
}

if [ $# -ne 0 ]; then
  if [ "$1" != "--no-ver" ]; then
    update_panel_version
  else
    echo "Will not increase version number."
  fi
else
  update_panel_version
fi

function compile_for_board_type() {
  BOARD_TYPE="$1"
  if [[ "$BOARD_TYPE" == "sonoff" ]]; then
    PLATFORMIO_ENV_NAME="original_sonoff"
  elif [[ "$BOARD_TYPE" == "custom" ]]; then
    PLATFORMIO_ENV_NAME="custom_pcb"
  else
    echo "Invalid board type."
    exit 1
  fi

  # Build firmware
  platformio run --environment "${PLATFORMIO_ENV_NAME}"
  firmware_build_result="$?"

  if [ "$firmware_build_result" -ne 0 ]; then
    echo "Firmware build failed. Will not upload to NSPanelManager"
    exit 1
  fi

  source ./build_image.sh "${BOARD_TYPE}"

  # Upload firmware to NSPanelManager
  echo "Uploading firmware for ${BOARD_TYPE}."
  curl http://"$NSPanelManager_address":"$NSPanelManager_port"/save_new_firmware -F firmware=@.pio/build/"${PLATFORMIO_ENV_NAME}"/firmware.bin -F "model=${BOARD_TYPE}"
  firmware_status="$?"

  echo "Uploading merged_flash.bin for ${BOARD_TYPE}."
  curl http://"$NSPanelManager_address":"$NSPanelManager_port"/save_new_merged_flash -F bin=@merged-flash.bin -F "model=${BOARD_TYPE}"
  merged_flash_status="$?"
  \rm -f merged-flash.bin # Cleanup after merged-flash has been built.

  if [ "$firmware_status" -eq 0 ] && [ "$merged_flash_status" -eq 0 ]; then
    echo "Firmware built and uploaded to manager."
  else
    echo "Something went wrong during upload, will not call OTA!"
  fi
}

compile_for_board_type "sonoff" # Compile and upload sonoff PCB firmware
compile_for_board_type "custom" # Compile and upload custom PCB firmware