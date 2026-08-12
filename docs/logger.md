# Logger interface

Applications should treat mudmux logging as part of the shared-library C ABI.
The host process does not need to use mudmux's logging implementation library,
inspect mudmux's internal logger, or coordinate with mudmux through process-global
logger state.

The public logging interface has two controls:

- `mudmux_set_log_level(int level)` sets mudmux's internal log threshold.
- `mudmux_register_logger_callback(callback, ctx)` routes mudmux log records to
  an application callback.

Log levels are passed as integer severity values:

| Level | Severity |
| --- | --- |
| `0` | trace |
| `1` | debug |
| `2` | info |
| `3` | warning |
| `4` | error |
| `5` | critical |
| `6` | off |

## Default case

If the application does not call any logger API, mudmux initializes its default
logger on first use. Log records are written to the library's default stderr
logger using mudmux's internal default level.

This is the simplest integration mode:

```c
#include <mudmux/mudmux.h>

if (!mudmux_init(config_yaml)) {
    /* mudmux has already logged the reason through its default logger. */
    return false;
}
```

In this mode the application observes logs as process stderr output only. It
should not assume anything about the logger object used internally.

## Setting the log level only

An application can keep the default log destination and change only mudmux's
internal log threshold:

```c
#include <mudmux/mudmux.h>

mudmux_set_log_level(1); /* debug */

if (!mudmux_init(config_yaml)) {
    return false;
}
```

Calling `mudmux_set_log_level()` alone does not install an application callback
and does not require the application to link against or configure mudmux's
logging implementation. It only changes which mudmux records pass the internal
threshold.

## Using a logger callback

Applications that want to capture mudmux log records directly can register a C
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
mudmux_set_log_level(2); /* info */

if (!mudmux_init(config_yaml)) {
    return false;
}
```

Register the callback before calling APIs whose log records you want to capture.
The callback may be registered before or after `mudmux_init()`, but records
emitted before registration will use the active logger at that time.

Passing a null callback is ignored and leaves the current mudmux logging route
unchanged.

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
| `level` | The mudmux integer severity value for the record. |
| `file` | Source filename for the mudmux log call site, or an empty string when source information is unavailable. |
| `line` | Source line for the mudmux log call site, or `0` when unavailable. |
| `func` | Source function name for the mudmux log call site, or an empty string when unavailable. |
| `msg` | The formatted log message payload. It does not include the logger pattern, timestamp, level label, or trailing newline. |

The string pointers are valid only for the duration of the callback. Copy any
data that must be retained after the callback returns.

## Lifetime and threading

The callback can run on whichever thread emits the log record: the event-loop
thread, worker threads in relaxed execution mode, console or file-input helper
threads, or the caller's thread for synchronous API calls. Application state
reached through `ctx` must be synchronized by the application.

Keep the callback brief. Logging can occur while mudmux is handling I/O or
tearing down resources, so blocking inside the callback can delay transport
progress or shutdown.

`mudmux_deinit()` releases any callback route owned by the current mudmux
instance. Register the callback again after reinitialization if records from the
new run should be routed through the application.

## Internal design

mudmux currently implements this interface with spdlog inside the shared
library. That dependency is an implementation detail rather than an
application-facing ABI requirement.

On first logging setup, mudmux creates a stderr logger if no callback logger has
been registered. `mudmux_set_log_level()` initializes logging if needed and then
updates the active mudmux log threshold.

`mudmux_register_logger_callback()` installs a multi-threaded callback sink as
the active mudmux logger and marks mudmux logging as initialized. A later call
to `mudmux_set_log_level()` changes the threshold without replacing the callback
route.

During `mudmux_deinit()`, mudmux replaces the active logger with a fresh stderr
logger. This releases callback sinks while keeping logging functional for later
code in the same process, including test teardown or subsequent API calls.

The current integer level values match the spdlog level ordering used
internally, but applications should treat those values as mudmux's C ABI
contract rather than as permission to inspect or configure spdlog state directly.
