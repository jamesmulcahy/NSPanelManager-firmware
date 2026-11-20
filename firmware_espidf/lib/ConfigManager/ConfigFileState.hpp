#pragma once
#include <string>

// The decisions behind loading and saving /littlefs/config.json, kept free of ESP-IDF headers so they can be
// built and tested on a host (test/host/config_file). ConfigManager does the file I/O and the locking.
class ConfigFileState {
public:
  // How load_config() went.
  enum class LoadOutcome {
    LOADED,     // Read and parsed.
    MISSING,    // No config.json: a new panel, or a partition that was just formatted.
    UNREADABLE, // There is a file, but it could not be read or parsed (empty, bad JSON, a short read, out of memory).
  };

  // What app_main() does with that.
  enum class BootAction {
    CONNECT,                    // Join the configured network.
    SAVE_DEFAULTS_AND_START_AP, // Write the defaults, so there is a file to replace, and start the setup AP.
    START_AP_WITHOUT_SAVING,    // Start the setup AP and leave the file alone: a failure to read it may be
                                // transient, and the defaults written over it would lose the panel's settings.
  };

  static BootAction boot_action(LoadOutcome outcome);

  // What save_config() does with the newly serialised config.
  enum class SaveAction {
    WRITE,             // Write it.
    SKIP_UNCHANGED,    // The file already holds exactly this.
    REFUSE_UNREADABLE, // The file could not be read at boot, and this is not a save the user asked for.
  };

  /**
   * @param content: the config as save_config() would now write it
   * @param replace_unreadable: the save replaces whatever is on flash, readable or not (the web UI's save and
   * factory reset). Other saves leave a file that could not be read at boot alone.
   */
  SaveAction save_action(const std::string &content, bool replace_unreadable) const;

  // After a successful load: `content` is the loaded config serialised as save_config() would write it.
  void loaded(std::string content);
  // After a load that found no file.
  void missing();
  // After a load that found a file but could not use it.
  void unreadable();
  // After `content` was written and closed without error.
  void saved(std::string content);

  // The file could not be read at boot and nothing has replaced it since.
  bool is_unreadable() const { return _unreadable; }

private:
  std::string _last;     // What the file holds, as far as this boot knows
  bool _known = false;   // _last is valid
  bool _unreadable = false;
};
