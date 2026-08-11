# Logger callback

mudmux logs through spdlog. By default, the library initializes a standard
stderr logger the first time `mudmux_set_log_level()` is called. Hosted logic
that wants to receive mudmux log records directly can instead register a C
callback:

```c
#include <mudmux/mudmux.h>

static void on_log(
    void *context,
    int level,
    const char *file,
    int line,
    const char *function,
    const char *message) {
    /* Copy message here if it must outlive this callback. */
}

mudmux_register_logger_callback(on_log, application_logger);
mudmux_set_log_level(2); /* spdlog::level::info */
```

`mudmux_register_logger_callback()` installs a `spdlog::sinks::callback_sink_mt`
as spdlog's default logger and marks mudmux logging as initialized. A later
call to `mudmux_set_log_level()` only changes the active spdlog level; it does
not replace the callback logger.

## Callback arguments

The callback type is:

```c
typedef void (*mudmux_logger_callback_t)(
    void *ctx,
    int level,
    const char *file,
    int line,
    const char *func,
    const char *msg);
```

| Argument | Contract |
| --- | --- |
| `ctx` | The context pointer supplied to `mudmux_register_logger_callback()`. mudmux stores and forwards the pointer value but does not inspect, copy, or free it. |
| `level` | The integer value of the spdlog level for the record. This matches `spdlog::level::level_enum`. |
| `file` | Source filename from the spdlog call site, or an empty string when spdlog did not provide source information. |
| `line` | Source line from the spdlog call site, or `0` when unavailable. |
| `func` | Source function name from the spdlog call site, or an empty string when unavailable. |
| `msg` | The formatted log message payload. It does not include the logger pattern, timestamp, level label, or trailing newline. |

The string pointers are valid only for the duration of the callback. Copy any
data that must be retained after the callback returns.

## Lifetime and threading

Register the logger callback before calling APIs whose log records you want to
capture. It may be registered before or after `mudmux_init()`.

The callback can run on whichever thread emits the log record: the event-loop
thread, worker threads in relaxed execution mode, console or file-input helper
threads, or the caller's thread for synchronous API calls. The installed sink is
multi-threaded, but application state reached through `ctx` must still be
synchronized by the application.

Keep the callback brief. Logging can occur while mudmux is handling I/O or
tearing down resources, so blocking inside the callback can delay transport
progress or shutdown.

## Shutdown

`mudmux_deinit()` replaces the active spdlog default logger with a fresh stderr
logger, releasing any callback sinks. After deinitialization, spdlog remains
functional so that code running after `mudmux_deinit()` (such as test teardown
or subsequent API calls) can still log safely.

`mudmux_init()` installs a new stderr logger when `spdlog_initialized` is
false, so the pattern `mudmux_deinit()` → `mudmux_init()` is safe within the
same process. Register the callback again after `mudmux_init()` if log records
from the new run should be routed through the application.

Passing a null callback is ignored and leaves the current spdlog default logger
unchanged.
