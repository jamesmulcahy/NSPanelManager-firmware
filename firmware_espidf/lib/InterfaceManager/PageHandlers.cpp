#include <PageHandlers.hpp>
#include <esp_log.h>

esp_err_t PageHandlers::add(esp_event_base_t event_base, int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg) {
  return this->add_to(NULL, event_base, event_id, event_handler, event_handler_arg);
}

esp_err_t PageHandlers::add_to(esp_event_loop_handle_t event_loop, esp_event_base_t event_base, int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg) {
  Registration registration = {
      .event_loop = event_loop,
      .event_base = event_base,
      .event_id = event_id,
      .instance = NULL,
  };
  esp_err_t result = event_loop == NULL
                         ? esp_event_handler_instance_register(event_base, event_id, event_handler, event_handler_arg, &registration.instance)
                         : esp_event_handler_instance_register_with(event_loop, event_base, event_id, event_handler, event_handler_arg, &registration.instance);
  if (result != ESP_OK) {
    return result;
  }

  bool kept = false;
  bool retired;
  portENTER_CRITICAL(&this->_lock);
  retired = this->_retired;
  if (!retired && this->_count < MAX_REGISTRATIONS) {
    this->_registrations[this->_count++] = registration;
    kept = true;
  }
  portEXIT_CRITICAL(&this->_lock);

  if (!kept) {
    // Either the page was unshown while this show() was still registering, or
    // MAX_REGISTRATIONS is too small. Do not leave a handler nobody owns.
    PageHandlers::_unregister(registration);
    if (!retired) {
      ESP_LOGE("PageHandlers", "More than %zu handlers registered for one page; raise MAX_REGISTRATIONS.", MAX_REGISTRATIONS);
      return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

void PageHandlers::retire() {
  std::array<Registration, MAX_REGISTRATIONS> registrations;
  size_t count;
  portENTER_CRITICAL(&this->_lock);
  this->_retired = true;
  registrations = this->_registrations;
  count = this->_count;
  this->_count = 0;
  portEXIT_CRITICAL(&this->_lock);

  for (size_t i = 0; i < count; i++) {
    PageHandlers::_unregister(registrations[i]);
  }
}

void PageHandlers::_unregister(const Registration &registration) {
  esp_err_t result = registration.event_loop == NULL
                         ? esp_event_handler_instance_unregister(registration.event_base, registration.event_id, registration.instance)
                         : esp_event_handler_instance_unregister_with(registration.event_loop, registration.event_base, registration.event_id, registration.instance);
  if (result != ESP_OK) {
    ESP_LOGW("PageHandlers", "Failed to unregister page event handler: %s", esp_err_to_name(result));
  }
}
