#pragma once

/// Registers esp-tls's stack as a no-op one. Call once at boot, before anything creates a connection.
void register_no_tls_stack();
