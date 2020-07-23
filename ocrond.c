/* See LICENSE file for copyright and license details. */

/* Mostly Posix.1-2008 compatible, but also relies on the following extensions:
 * reallocarray(3), ffsl(3), ffsll(3). */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

#define VERSION "0.10"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define VALID_HOUR(job, hour) ((job).hours >> (hour) & 1)
#define VALID_MDAY(job, mday) ((job).mdays >> (mday) & 1)
#define VALID_WDAY(job, wday) ((job).wdays >> (wday) & 1)
#define VALID_DAY(job, mday, wday) (VALID_MDAY(job, mday) || VALID_WDAY(job, wday))
#define VALID_MONTH(job, month) ((job).months >> (month) & 1)
#define VALID_DATE(job, mday, wday, month) (VALID_DAY(job, mday, wday) && VALID_MONTH(job, month))

struct Job
{
	long long minutes;
	time_t time;
	char *command;
	long hours;
	long mdays;
	pid_t pid;
	short months;
	short wdays;
	short lineno;
};

enum Event { REACHED_TARGET, SKIPPED_TARGET, CAUGHT_SIGNAL, TIME_CHANGED, NOTHING_HAPPENED };

static const char *no_aliases[] = { NULL };
static const char *months_aliases[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL
};
static const char *wdays_aliases[] = {
	"Sun", "Mon", "Tue", "Wed",
	"Thu", "Fri", "Sat", NULL
};

static sig_atomic_t hasZombies;

/* A queue containing all jobs. Implemented as a simple unordered array. */
static int capJobs;
static int numJobs;
static struct Job *jobs;

/* A pointer to the character that is currently examined
 * by the crontab parser. Only used at startup. */
static char *text;
/* A pointer to the end of the line that we currently parse.
 * Only used at startup. */
static char *eol;

/* General utility functions. */

/* Similar to read(2), but automatically restarts if less than count
 * bytes were read or if EINTR, EAGAIN, or EWOULDBLOCK occurred. */
static int
readall(int fd, void *buf, size_t count)
{
	ssize_t ret;
	while (count > 0) {
		ret = read(fd, buf, count);
		if (ret < 0) {
			if (errno != EINTR && errno != EAGAIN &&
			    errno != EWOULDBLOCK) return -1;
			ret = 0;
		}
		buf += ret, count -= ret;
	}
	return 0;
}

/* Similar to write(2), but automatically restarts if less than count
 * bytes were written or if EINTR, EAGAIN, or EWOULDBLOCK occurred. */
static int
writeall(int fd, const void *buf, size_t count)
{
	ssize_t ret;
	while (count > 0) {
		ret = write(fd, buf, count);
		if (ret < 0) {
			if (errno != EINTR && errno != EAGAIN &&
			    errno != EWOULDBLOCK) return -1;
			ret = 0;
		}
		buf += ret, count -= ret;
	}
	return 0;
}

/* Open a unique close-on-exec temporary file for reading & writing
 * that is not visible from the file system. 
 * The template argument is the same as for mkstemp(3), except that
 * it won't be overwritten. */
static int
mkanonfile(const char *template)
{
	char name[128];
	int fd, flags;

	assert(strlen(template) < sizeof(name));
	strcpy(name, template);
	
	if ((fd = mkstemp(name)) < 0) {
		/* TODO pull out error message stuff. */
		syslog(LOG_EMERG, "Can't open temp file: %m");
		return -1;
	}

	/* Why isn't mkostemp(3) part of POSIX? Argh ... */
	/* At least I'm pretty sure we don't have to check for errors here. */
	flags = fcntl(fd, F_GETFD);
	fcntl(fd, F_SETFD, flags & FD_CLOEXEC);
	
	if (unlink(name) < 0) {
		/* TODO pull out error message stuff. */
		syslog(LOG_EMERG, "Can't unlink temp file: %m");
		close(fd);
		return -1;
	}

	return fd;
}

/* Just like glibc's strchrnul(3), but portable.
 * Essentially, it behaves just like strchr(3), but it will
 * also return any NUL characters encountered along the way. */
static char *
pstrchrnul(const char *str, int ch)
{
	while (*str && *str != ch) ++str;
	/* Hacky, but mandated by the function signature. */
	return (char *) str;
}

/* Returns 0 or 1 depending on whether year is a leap year in the Gregorian calendar or not.
 * year must contain the actual year, without offset. */
static int
is_leap_year(int year)
{
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* Computes how many days there are in a particular month of the Gregorian calendar.
 * month may be in the range 0 to 11 inclusive, and year must contain the actual year,
 * without offset. */
static int
days_in_month(int month, int year)
{
	return month != 1 ? 30 + ((month % 7 + 1) & 1) : 28 + is_leap_year(year);
}

/* Exit with a warning message. Should only be called during initialization! */
static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(LOG_EMERG, fmt, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

static char *
read_file(const char *filename)
{
	struct stat info;
	char *contents;
	int fd;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		die("Can't open %s: %m", filename);
	if (fstat(fd, &info) < 0)
		die("Can't stat %s: %m", filename);
	if ((contents = malloc(info.st_size + 1)) == NULL)
		die("Out of memory.");
	if (readall(fd, contents, info.st_size) < 0)
		die("Can't read %s: %m", filename);
	contents[info.st_size] = 0;
	close(fd);

	return contents;
}

/* Job time-finding algorithm. */
static void
update_job(int idx, time_t now)
{
	struct tm tm;
	struct Job job;
	long long minutes_left;
	long hours_left;
	int today_alright, lookahead = 0;

	job = jobs[idx];

	localtime_r(&now, &tm);
	tm.tm_sec = 0;
	tm.tm_isdst = -1;

	today_alright = VALID_DATE(job, tm.tm_mday, tm.tm_wday, tm.tm_mon);

	/* Determine minute, and exit early if possible. */
	assert(job.minutes != 0);
	if (today_alright && VALID_HOUR(job, tm.tm_hour)) {
		++tm.tm_min;
		minutes_left = job.minutes & ~0ULL << tm.tm_min;
		if (minutes_left != 0LL) {
			tm.tm_min = ffsll(minutes_left) - 1;
			goto finished;
		}
	}
	tm.tm_min = ffsll(job.minutes) - 1;

	/* Determine hour, and exit early if possible. */
	assert(job.hours != 0);
	if (today_alright) {
		++tm.tm_hour;
		hours_left = job.hours & ~0UL << tm.tm_hour;
		if (hours_left != 0L) {
			tm.tm_hour = ffsl(hours_left) - 1;
			goto finished;
		}
	}
	tm.tm_hour = ffsl(job.hours) - 1;

	/* Determine day, month, and year. */
	do {
		if (++lookahead > (MAX_LOOKAHEAD)) {
			syslog(LOG_WARNING, "Job '%s' exceeded the maximum lookahead and will be ignored.", job.command);
			jobs[idx] = jobs[--numJobs];
			return;
		}

		tm.tm_wday = (tm.tm_wday + 1) % 7;
		if (++tm.tm_mday > days_in_month(tm.tm_mon, 1900 + tm.tm_year)) {
			tm.tm_mday = 1;
			if (++tm.tm_mon >= 12) {
				tm.tm_mon = 0;
				++tm.tm_year;
			}
		}
	} while (!VALID_DATE(job, tm.tm_mday, tm.tm_wday, tm.tm_mon));

finished:
	jobs[idx].time = mktime(&tm);
}

static int
closest_job(void)
{
	int idx, next = 0;
	
	if (!numJobs) return -1;
	for (idx = 1; idx < numJobs; ++idx) {
		if (jobs[idx].time < jobs[next].time)
			next = idx;
	}

	return next;
}

/* Crontab parsing. */

static int
eat_char(char c)
{
	int r;
	
	r = *text == c;
	if (r) ++text;
	return r;
}

static int
skip_space(void)
{
	if (!isblank(*text)) return -1;
	do ++text; while (isblank(*text));
	return 0;
}

static int
parse_number(int *number)
{
	int num = 0;

	if (!isdigit(*text)) return -1;
	do {
		num = num * 10 + *text++ - '0';
	} while (isdigit(*text));
	*number = num;

	return 0;
}

static int
parse_value(const char *aliases[], int *number)
{
	size_t len;
	int i;

	if (isdigit(*text)) {
		if (parse_number(number) < 0) return -1;
		return 0;
	}
	for (i = 0; aliases[i] != NULL; ++i) {
		len = strlen(aliases[i]);
		if (strncasecmp(text, aliases[i], len) == 0) {
			text += len;
			*number = i;
			return 0;
		}
	}

	return -1;
}

static int
parse_range(int min, int max, const char *aliases[], long long *field)
{
	int first, last, step = 1, i;

	if (eat_char('*')) {
		if (eat_char('/')) {
			if (parse_number(&step) < 0) return -1;
			if (step < 1) return -1;
			for (i = min; i <= max; i += step) {
				*field |= 1ULL << i;
			}
		}

		return 0;
	} else {
		if (parse_value(aliases, &first) < 0) return -1;
		last = first;
		if (eat_char('-')) {
			if (parse_value(aliases, &last) < 0) return -1;
			if (eat_char('/')) {
				if (parse_number(&step) < 0) return -1;
			}
		}

		if (first > last) return -1;
		if (first < min) return -1;
		if (last > max) return -1;
		if (step < 1) return -1;
		for (i = first; i <= last; i += step) {
			*field |= 1ULL << i;
		}

		return 0;
	}
}

static int
parse_field(int min, int max, const char *aliases[], long long *field)
{
	*field = 0LL;
	do {
		if (parse_range(min, max, aliases, field) < 0) return -1;
	} while (eat_char(','));
	if (skip_space() < 0) return -1;
	return 0;
}

static int
parse_command(char **command)
{
	int esc = 0;
	char *out, c;

	/* Guard against empty commands. */
	if (text >= eol) return -1;
	
	if ((*command = malloc(eol - text + 2)) == NULL) {
		die("Out of memory.");
	}

	/* Decode escaped string into its raw form. */
	out = *command;
	while (text < eol) {
		c = *text++;
		if (c == '%') {
			if (esc) --out;
			else c = '\n';
		}
		*out++ = c;
		esc = c == '\\';
	}
	/* NUL-terminate the string. */
	*out = 0;
	
	/* Split command from stdin data. */
	*pstrchrnul(*command, '\n') = 0;

	return 0;
}

static int
parse_line(int lineno)
{
	struct Job job;
	long long field;

	memset(&job, 0, sizeof(job));
	job.lineno = lineno;

	/* We don't care if we actually find spaces here or not. */
	skip_space();
	
	/* Dismiss empty lines and comments. */
	if (*text == '#') return 0;
	if (!*text || *text == '\n') return 0;
	
	if (parse_field(0, 59, no_aliases, &field) < 0) return -1;
	job.minutes = field;

	if (parse_field(0, 23, no_aliases, &field) < 0) return -1;
	job.hours = field;

	if (parse_field(1, 31, no_aliases, &field) < 0) return -1;
	job.mdays = field;

	if (parse_field(0, 11, months_aliases, &field) < 0) return -1;
	job.months = field;

	if (parse_field(0, 7, wdays_aliases, &field) < 0) return -1;
	job.wdays = field;

	if (parse_command(&job.command) < 0) return -1;

	/* Fill in unrestricted fields. */
	if (!job.minutes) job.minutes = ~0LL;
	if (!job.hours) job.hours = ~0L;
	if (!job.months) job.months = ~0;
	job.wdays |= job.wdays >> 7 & 1;
	if (!job.mdays && !job.wdays) {
		job.mdays = ~0L;
	}

	/* Add the job to the list and we're done. */
	if (numJobs >= capJobs) {
		capJobs = capJobs ? 2 * capJobs : 4;
		jobs = reallocarray(jobs, capJobs, sizeof(jobs[0]));
		if (jobs == NULL) die("Out of memory.");
	}
	jobs[numJobs++] = job;

	return 0;
}

static void
parse_file(const char *filename)
{
	char *contents;
	int lineno = 1;

	contents = read_file(filename);
	text = contents;
	do {
		eol = pstrchrnul(text, '\n');
		if (parse_line(lineno) < 0) {
			syslog(LOG_WARNING, "Line %d of %s will be ignored because of bad syntax.\n", lineno, filename);
		}
		text = eol + 1;
		++lineno;
	} while (*eol);
	text = NULL;
	free(contents);
}

/* Create a temporary stdin file for a specific job. */
static int
create_infile(int idx)
{
	char *indata;
	size_t inlen;
	int infd;

	indata = strchr(jobs[idx].command, 0) + 1;
	inlen = strlen(indata);

	if (inlen) {
		if ((infd = mkanonfile(STDIN_TEMP)) < 0) {
			return -1;
		}
		if (writeall(infd, indata, inlen) < 0) {
			syslog(LOG_EMERG, "Can't write to temp file: %m");
			close(infd);
			return -1;
		}
		/* Don't need to check for errors here. */
		lseek(infd, 0, SEEK_SET);
	} else {
		if ((infd = open("/dev/null", O_RDONLY | O_CLOEXEC)) < 0) {
			syslog(LOG_EMERG, "Can't open /dev/null for reading: %m");
			return -1;
		}
	}
	return infd;
}

/* Execute a job. */
static void
run_job(int idx)
{
	pid_t pid;
	int infd;

	/* Only execute the job if it isn't currently running. */
	if (jobs[idx].pid) {
		syslog(LOG_WARNING, "Job #%d won't be executed since it is still running.", jobs[idx].lineno);
		return;
	}

	switch (pid = fork()) {
	case -1:
		syslog(LOG_EMERG, "Cannot start a new process: %m");
		return;

	case 0:
		if ((infd = create_infile(0)) < 0)
			goto child_failed;
		setpgid(0, 0);
		dup2(infd, STDIN_FILENO);
		execl(SHELL, SHELL, "-c", jobs[idx].command, NULL);
		/* If we reach this line, execl() must have failed. */
	child_failed:
		exit(137);

	default:
		syslog(LOG_NOTICE, "Executing job #%d with pid %d.", jobs[idx].lineno, pid);
		jobs[idx].pid = pid;
		return;
	}
}

/* Sleep until an event arrives. */
static enum Event
wait_for_event(time_t *now, time_t *target)
{
	struct timespec spec;
	time_t past;

	if (target != NULL) {
		if (*target > *now) {
			past = *now;
			spec.tv_sec = MIN(*target - *now, (WAKEUP_PERIOD) * 60);
			spec.tv_nsec = 0L;
			if (nanosleep(&spec, NULL) < 0) return CAUGHT_SIGNAL;
			time(now);
			if (*now < past) return TIME_CHANGED;
			return NOTHING_HAPPENED;
		} else {
			return *now - *target <= (CATCHUP_LIMIT) * 60 ? REACHED_TARGET : SKIPPED_TARGET;
		}
	} else {
		pause();
		return CAUGHT_SIGNAL;
	}
}

/* Reap (and log) any zombie childs that have piled up since the last reap. */
static void
reap_zombies(void)
{
	pid_t pid;
	int status, idx;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		/* Log the return status of the child. */
		if (WIFEXITED(status)) {
			syslog(LOG_NOTICE, "pid %d returned with status %d.", pid, WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			syslog(LOG_WARNING, "pid %d terminated by signal %s.", pid, strsignal(WTERMSIG(status)));
		} else if (WIFSTOPPED(status)) {
			syslog(LOG_WARNING, "pid %d stopped by signal %s.", pid, strsignal(WSTOPSIG(status)));
		} else continue;

		/* Allow the returning job to be ran again. */
		for (idx = 0; idx < numJobs; ++idx) {
			if (jobs[idx].pid == pid) {
				jobs[idx].pid = 0;
				break;
			}
		}
		
		/* This shouldn't ever happen, I don't think? */
		if (idx == numJobs) {
			syslog(LOG_WARNING, "pid %d was my child but wasn't executed by me!", pid);
		}
	}
}

static void
sigchld_handler(int signal)
{
	(void) signal;
	hasZombies = 1;
}

int
main()
{
	time_t now;
	int i, next;

	struct sigaction sa = { 0 };
	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);

	openlog(LOGIDENT, LOG_CONS, LOG_CRON);
	syslog(LOG_NOTICE, "ocron %s starting up.", VERSION);
	if (!(access(CRONTAB, F_OK) < 0)) {
		parse_file(CRONTAB);
	}
	time(&now);

restart:
	for (i = numJobs - 1; i >= 0; --i) update_job(i, now);
	next = closest_job();
	for (;;) {
		if (hasZombies) {
			reap_zombies();
			hasZombies = 0;
		}
		switch (wait_for_event(&now, next < 0 ? NULL : &jobs[next].time)) {
		case REACHED_TARGET:
			run_job(next);
			update_job(next, now);
			next = closest_job();
			break;
		case SKIPPED_TARGET:
			syslog(LOG_NOTICE, "Job #%d had to be skipped because it was too far "
				"in the past. (Was the system time set forward?)", jobs[next].lineno);
			update_job(next, now);
			next = closest_job();
			break;
		case TIME_CHANGED:
			syslog(LOG_NOTICE, "Detected that the system time was set back. Recalculating.");
			goto restart;
		default:
			break;
		}
	}
}
