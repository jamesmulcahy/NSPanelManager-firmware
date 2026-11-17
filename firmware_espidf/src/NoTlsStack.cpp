// The firmware is built with ESP_TLS_CUSTOM_STACK and no TLS library (mbedTLS TLS is off), which
// saves about 65 KB of flash. esp-tls still runs plain TCP connections (MQTT, esp_http_client,
// esp_https_ota over http://) but calls the registered stack's net_init() when it creates a
// connection and conn_delete() when it destroys one, and logs "No TLS stack registered" at ERROR
// for each call if none is registered. This registers a stack that does nothing for those calls and
// refuses every TLS operation, so MQTT over TLS and https:// fail cleanly instead of crashing.
#include "NoTlsStack.hpp"

#include <sdkconfig.h>

#if CONFIG_ESP_TLS_CUSTOM_STACK

#include <esp_log.h>
#include <esp_tls.h>
#include <esp_tls_custom_stack.h>

static const char *TAG = "NoTlsStack";

static esp_err_t create_ssl_handle(void *, const char *, size_t, const void *, esp_tls_t *, void *) { return ESP_ERR_NOT_SUPPORTED; }
static int handshake(void *, esp_tls_t *, const esp_tls_cfg_t *) { return -1; }
static ssize_t tls_read(void *, esp_tls_t *, char *, size_t) { return -1; }
static ssize_t tls_write(void *, esp_tls_t *, const char *, size_t) { return -1; }
static void conn_delete(void *, esp_tls_t *) {}
static void net_init(void *, esp_tls_t *) {}
static void *get_ssl_context(void *, esp_tls_t *) { return nullptr; }
static ssize_t get_bytes_avail(void *, esp_tls_t *) { return 0; }
static esp_err_t init_global_ca_store(void *) { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t set_global_ca_store(void *, const unsigned char *, const unsigned int) { return ESP_ERR_NOT_SUPPORTED; }
static void *get_global_ca_store(void *) { return nullptr; }
static void free_global_ca_store(void *) {}
static const int *get_ciphersuites_list(void *) { return nullptr; }

static const esp_tls_stack_ops_t NO_TLS_OPS = {
    .version = ESP_TLS_STACK_OPS_VERSION,
    .create_ssl_handle = create_ssl_handle,
    .handshake = handshake,
    .read = tls_read,
    .write = tls_write,
    .conn_delete = conn_delete,
    .net_init = net_init,
    .get_ssl_context = get_ssl_context,
    .get_bytes_avail = get_bytes_avail,
    .init_global_ca_store = init_global_ca_store,
    .set_global_ca_store = set_global_ca_store,
    .get_global_ca_store = get_global_ca_store,
    .free_global_ca_store = free_global_ca_store,
    .get_ciphersuites_list = get_ciphersuites_list,
    // Optional: client sessions, server sessions and the crypto helpers are not used.
    .get_client_session = nullptr,
    .free_client_session = nullptr,
    .server_session_ticket_ctx_init = nullptr,
    .server_session_ticket_ctx_free = nullptr,
    .server_session_create = nullptr,
    .server_session_init = nullptr,
    .server_session_continue_async = nullptr,
    .server_session_delete = nullptr,
    .crypto_sha1 = nullptr,
    .crypto_base64_encode = nullptr,
};

void register_no_tls_stack() {
  esp_err_t res = esp_tls_register_stack(&NO_TLS_OPS, nullptr);
  if (res != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register the no-op TLS stack: %s", esp_err_to_name(res));
  }
}

#else

void register_no_tls_stack() {}

#endif
