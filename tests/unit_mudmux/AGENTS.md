# mudmux Shared Library unit tests

The `unit_mudmux` target deliberately tests `mudmux` as a shared library black box.
It links against the shared library as a separate binary and should exercise behavior
only through public headers and the exported C ABI.

Do not rely on implementation details from inside the shared library:

- Do not include private headers or call non-exported helpers.
- Do not assert on process-global implementation state owned by dependencies inside
  the library, such as spdlog's default logger or global log level.
- For logger callback tests, trigger messages through exported `mudmux` APIs
  (for example, an invalid `mudmux_init()` configuration) and assert only the
  callback payload exposed through the C ABI.

The test program may use C++ libraries such as GoogleTest or spdlog for its own
test scaffolding, but those uses must not become expectations about mudmux's
internal implementation state.
