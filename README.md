# ocron, the resource-friendly cron implementation

## Why ocron?

To the best of the authors knowledge, **ocron** is the *only* cron implementation available that *does not need to wake up every single minute*, potentially reducing power consumption etc.
Instead, **ocron** only either wakes up when it wants to execute a job, or if a user-configurable amount of time has elapsed (usually a whole hour),
just to make sure the system time wasn't changed dramatically.

Also, it looks like **ocron** uses quite a bit less memory than other cron implementations - This *might* make a difference on very low-power (embedded) devices.

## Supported syntax

**ocron** understands all the usual syntax features and common extensions that you've come to expect from a cron daemon:

- Both comments and blank lines are allowed.
- Rules are of the following form: `minutes hours month-days months week-days command`
- In the first 5 fields, '\*' means that the field is unspecified, '-' can be used for inclusive ranges, '/' after '\*' or after a range specifies periods.
- For the `months` and `week-days` fields, 3-letter case-insensitive aliases may be used (for example: `Jan`, `JUL`, `aug`).
- In the `week-days` field, 0 and 7 both mean Sunday.
- Commands are executed if *either* the `month-day` *or* the `week-day` matches the current day.
- Rules can contain data to be sent to the commands stdin through the usual percent sign syntax.
- Of course, the percent sign can also be escaped by preceding it with a backslash.

If a rule contains syntax errors and cannot be parsed, it is simply ignored (with a warning message), so other rules will still execute just fine.

## How to install

You only need `make` and a C99 / POSIX.1.2008 compatible C compiler (like GCC).
Simply `cd` into this directory, run `make`, and then as superuser run `make install`.
You can edit the files `config.mk` and `config.h` to adapt the build settings to your system.
The Makefile honors both `PREFIX` and `DESTDIR`.

