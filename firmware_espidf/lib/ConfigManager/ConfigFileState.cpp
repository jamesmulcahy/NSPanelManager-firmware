#include "ConfigFileState.hpp"
#include <utility>

ConfigFileState::BootAction ConfigFileState::boot_action(LoadOutcome outcome) {
  switch (outcome) {
  case LoadOutcome::LOADED:
    return BootAction::CONNECT;
  case LoadOutcome::MISSING:
    return BootAction::SAVE_DEFAULTS_AND_START_AP;
  case LoadOutcome::UNREADABLE:
  default:
    return BootAction::START_AP_WITHOUT_SAVING;
  }
}

ConfigFileState::SaveAction ConfigFileState::save_action(const std::string &content, bool replace_unreadable) const {
  if (_unreadable && !replace_unreadable) {
    return SaveAction::REFUSE_UNREADABLE;
  }
  if (_known && content == _last) {
    return SaveAction::SKIP_UNCHANGED;
  }
  return SaveAction::WRITE;
}

void ConfigFileState::loaded(std::string content) {
  _last = std::move(content);
  _known = true;
  _unreadable = false;
}

void ConfigFileState::missing() {
  _last.clear();
  _known = false;
  _unreadable = false;
}

void ConfigFileState::unreadable() {
  // What is on flash is unknown, so the first save allowed to replace it always writes.
  _last.clear();
  _known = false;
  _unreadable = true;
}

void ConfigFileState::saved(std::string content) {
  _last = std::move(content);
  _known = true;
  _unreadable = false;
}
