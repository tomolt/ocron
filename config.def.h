/* See LICENSE file for copyright and license details. */

/* The file that contains the cron rules. */
#define CRONTAB       "/etc/crontab"
/* The shell that should be executed to run a command. */
#define SHELL         "/bin/sh"
/* The name that should be used to refer to ocrond in the system log. */
#define LOGIDENT      "crond"

/* The maximum amount of time in minutes that may pass before ocrond has to wake up again.
 * These forced wake-ups are only neccessary to detect system clock changes.
 * If your system clock never changes, or your jobs run frequently enough and don't need
 * precision, you can safely make this value as large as you want. */
#define WAKEUP_PERIOD 60
/* How many minutes a scheduled job may lie in the past before it gets skipped. */
#define CATCHUP_LIMIT 60
/* The maximum amount of days that ocron may look into the future to schedule a job.
 * Should ocron be unable to schedule the job within this time frame, the job is
 * considered to be unable to execute and subsequently gets permanently disabled. */
#define MAX_LOOKAHEAD 2000

