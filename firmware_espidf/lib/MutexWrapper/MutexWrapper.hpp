#pragma once
#include <esp_event.h> // Included for base definitions like portMAX_DELAY and such
#include <freertos/semphr.h>
#include <utility>

template <typename T>
class MutexWrapped {
public:
  MutexWrapped() : _value{} {
    this->_mutex = portMUX_INITIALIZER_UNLOCKED;
  }

  MutexWrapped(T value) {
    this->_mutex = portMUX_INITIALIZER_UNLOCKED;
    this->_value = value;
  }

  T get() {
    portENTER_CRITICAL(&this->_mutex);
    T current_value = this->_value;
    portEXIT_CRITICAL(&this->_mutex);
    return current_value;
  }

  void set(T new_value) {
    portENTER_CRITICAL(&this->_mutex);
    std::swap(this->_value, new_value);
    portEXIT_CRITICAL(&this->_mutex);
    // new_value now holds the previous value and is destroyed here, outside the critical section. If T is
    // a shared_ptr holding the last reference, its deleter (e.g. freeing a protobuf tree) does not run
    // with interrupts disabled.
  }

private:
  T _value;
  portMUX_TYPE _mutex;
};