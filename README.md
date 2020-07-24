# ocron, the resource-friendly cron implementation

## Why ocron?

To the best of the authors knowledge, **ocron** is the *only* cron implementation available that *does not need to wake up every single minute*.
Instead, **ocron** only either wakes up when it wants to execute a job, or if a user-configurable amount of time has elapsed (usually a whole hour),
to make sure the system time wasn't changed much in the mean time.
This way, **ocron** can reduce overall power consumption and CPU time spent.

Also, it looks like **ocron** uses less memory than other cron implementations - This *might* make a difference on very low-power (embedded) devices.

## Supported syntax

**ocron** understands all the usual syntax features and common extensions that you've come to expect from a cron daemon:

- Both comments and blank lines are allowed.
- Rules are of the following form:
  ```
  minutes   hours   month-days   months   week-days   command
  ```
- In the first 5 fields, '\*' means that the field is unspecified, '-' can be used for inclusive ranges, and a '/' after a '\*' or after a range specifies a period.
- For the months and week-days fields, 3-letter case-insensitive aliases may be used (for example: `Jan`, `JUL`, `aug`).
- In the week-days field, 0 and 7 both mean Sunday.
- Commands are executed if *either* the month-day *or* the week-day matches the current day.

## Robustness

**ocron** should handle both system clock changes and Daylight Savings Time gracefully.

If a rule contains syntax errors and cannot be parsed, it is simply ignored (with a warning message), so other rules will still execute just fine.
Also, **ocron** will run correctly if no valid rules are specified or the crontab file doesn't exist.

After (re-) loading the crontab, **ocron** doesn't allocate any new memory, so memory leaks and out-of-memory situations can't arise.

A lot of effort has been made to keep **ocron** free of any signal-related race conditions.

## How to install

You only need `make` and a C99 / POSIX.1.2008 compatible C compiler (like GCC).
Simply change into this directory, run `make`, and then as superuser run `make install`.
You can edit the files `config.mk` and `config.h` to adapt the build settings to your system.
The Makefile honors both the `PREFIX` and `DESTDIR` environmental variables, used for packaging etc.

## How to run

If you want to run **ocron** at startup like any other daemon, you will have to write a service for your init system (systemd / Sys V init / runit / ...).
Note that **ocron** never daemonizes itself and always logs to the system log.
If you need it to run in the background, consider using Linux' `daemonize(1)` or FreeBSD's `daemon(1)`.
Most init systems want to do the daemonization themselves.

