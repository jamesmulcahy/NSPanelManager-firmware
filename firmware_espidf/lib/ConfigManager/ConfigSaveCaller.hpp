#pragma once

// What asked ConfigManager to save config.json, for nspanel_config_saves_by_caller_total{caller}.
// Every save_config() and change() call names one, so a write on flash can be traced to its cause.
enum ConfigSaveCaller {
  CONFIG_SAVE_BY_BOOT_DEFAULTS,   // No config.json at boot: the defaults are written to create it.
  CONFIG_SAVE_BY_BUTTONS,         // ButtonManager: button and relay settings from the manager's config.
  CONFIG_SAVE_BY_US_LAYOUT,       // LvglRenderer: the panel's US/EU layout from the manager's config.
  CONFIG_SAVE_BY_LOOK,            // LvglLook: a look chosen on the panel or sent by the manager.
  CONFIG_SAVE_BY_LOOK_CONFIRMED,  // LvglLook: the manager confirmed a look the panel reported.
  CONFIG_SAVE_BY_PANEL_NAME,      // NSPM_ConfigManager: the manager renamed the panel.
  CONFIG_SAVE_BY_TIMERS,          // TimerManager: countdown timers started, changed or ended.
  CONFIG_SAVE_BY_FIRMWARE_UPDATE, // UpdateManager: firmware checksums around a firmware update.
  CONFIG_SAVE_BY_GUI_UPDATE,      // UpdateManager: the TFT checksum after a GUI update.
  CONFIG_SAVE_BY_COMMAND,         // UpdateManager: a setting changed by a command (lvgl_ui, loop_watchdog).
  CONFIG_SAVE_BY_WEB_UI,          // WebManager: the panel's own setup page.
  CONFIG_SAVE_CALLER_COUNT
};
