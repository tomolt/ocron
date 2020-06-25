/* See LICENSE file for copyright and license details. */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define IS_LEAP_YEAR(year) ((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 400 == 0))
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
	short months;
	short wdays;
};

static const char *no_aliases[] = { NULL };
static const char *months_aliases[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL
};
static const char *wdays_aliases[] = {
	"Sun", "Mon", "Tue", "Wed",
	"Thu", "Fri", "Sat", NULL
};

/* A queue containing all jobs. Implemented as a binary heap. */
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

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(LOG_EMERG, fmt, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

static void *
emalloc(size_t size)
{
	void *mem;
	if ((mem = malloc(size)) == NULL) die("Out of memory.");
	return mem;
}

static void
reap_children(void)
{
	struct sigaction ign;
	ign.sa_handler = SIG_IGN;
	sigemptyset(&ign.sa_mask);
	ign.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT;
	sigaction(SIGCHLD, &ign, NULL);
}

static int
has_prefix(const char *str, const char *pfx)
{
	while (*pfx) {
		if ((*str | 0x20) != (*pfx | 0x20)) return 0;
		++str, ++pfx;
	}
	return 1;
}

static int
days_in_month(int month, int year)
{
	assert(month >= 0 && month < 12);
	if (month != 1) {
		return 30 + ((month % 7 + 1) & 1);
	} else {
		return 28 + IS_LEAP_YEAR(year);
	}
}

static char *
find_eol(char *line)
{
	while (*line && *line != '\n') ++line;
	return line;
}

static char *
read_file(const char *filename)
{
	struct stat info;
	char *contents;
	ssize_t off = 0, ret;
	int fd;

	fd = open(filename, O_RDONLY);
	if (fd < 0) die("Can't open %s: %m", filename);

	if (fstat(fd, &info) < 0) die("Can't stat %s: %m", filename);

	contents = emalloc(info.st_size + 1);
	contents[info.st_size] = 0;

	while (off < info.st_size) {
		ret = read(fd, contents + off, info.st_size - off);
		if (ret < 0) die("Can't read %s: %m", filename);
		off += ret;
	}

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

/* Restoring the structure of the job queue. */

static void
heapify_jobs(int idx)
{
	struct Job tmp;
	int left, right, min;

	for (;;) {
		left = 2 * idx + 1;
		right = left + 1;
		min = idx;
		if (left < numJobs && jobs[left].time < jobs[min].time) {
			min = left;
		}
		if (right < numJobs && jobs[right].time < jobs[min].time) {
			min = right;
		}
		if (min == idx) return;
		tmp = jobs[idx];
		jobs[idx] = jobs[min];
		jobs[min] = tmp;
		idx = min;
	}
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
	int i;

	if (isdigit(*text)) {
		if (parse_number(number) < 0) return -1;
		return 0;
	}
	for (i = 0; aliases[i] != NULL; ++i) {
		if (has_prefix(text, aliases[i])) {
			text += strlen(aliases[i]);
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
	
	*command = emalloc(eol - text + 2);

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
	/* NULL-terminate the string. */
	*out++ = 0;
	
	/* Split command from stdin data. */
	*find_eol(*command) = 0;

	return 0;
}

static int
parse_line(void)
{
	struct Job job;
	long long field;

	memset(&job, 0, sizeof(job));

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
		eol = find_eol(text);
		if (parse_line() < 0) {
			syslog(LOG_WARNING, "Line %d of %s will be ignored because of bad syntax.\n", lineno, filename);
		}
		text = eol + 1;
		++lineno;
	} while (*eol);
	text = NULL;
	free(contents);
}

/* Job execution & main loop. */

static void
fake_stdin(int idx)
{
	char *inname, *indata;
	size_t inlen;
	ssize_t ret;
	int infd;

	/* Write input data to a temporary file and make stdin point to it. */
	inname = strdup(STDIN_TEMP);
	if (inname == NULL) die("strdup: %m");
	infd = mkstemp(inname);
	if (infd < 0) die("mkstemp: %m");
	if (unlink(inname) < 0) die("unlink: %m");
	indata = strchr(jobs[idx].command, 0) + 1;
	inlen = strlen(indata);
	while (inlen > 0) {
		ret = write(infd, indata, inlen);
		if (ret < 0) die("write: %m");
		inlen -= ret;
		indata += ret;
	}
	lseek(infd, 0, SEEK_SET);
	dup2(infd, STDIN_FILENO);
	close(infd);
}

static void
run_job(int idx)
{
	pid_t pid;

	/* Fork and return immediately in the parent process. */
	pid = fork();
	if (pid < 0) syslog(LOG_EMERG, "fork: %m");
	if (pid != 0) return;

	fake_stdin(idx);

	setpgid(0, 0);
	execl(SHELL, SHELL, "-c", jobs[idx].command, NULL);
	/* If we reach this code, execl() must have failed. */
	die("execl: %m", jobs[idx].command);
}

int
main()
{
	time_t now, target, start;
	int i;

	openlog(LOGIDENT, LOG_CONS, LOG_CRON);
	reap_children();
	/* Yes, there's a race condition here, but all you can achieve with it
	 * is making ocrond exit on startup - there's easier ways to do that anyway. */
	if (!(access(CRONTAB, F_OK) < 0)) {
		parse_file(CRONTAB);
	}

recalculate:
	time(&now);
	for (i = numJobs - 1; i >= 0; --i) update_job(i, now);
	for (i = numJobs / 2 - 1; i >= 0; --i) heapify_jobs(i);
	for (;;) {
		if (!numJobs) {
			syslog(LOG_DEBUG, "No jobs found. Idling indefinitely.");
			pause();
			continue;
		}

		target = jobs[0].time;
		start = time(&now);
		for (;;) {
			if (now < start) {
				syslog(LOG_NOTICE, "Detected that the system time was set back significantly. Recalculating.");
				goto recalculate;
			}
			if (now >= target) break;
			sleep(MIN(target - now, (WAKEUP_PERIOD) * 60));
			time(&now);
		}

		if (now - target < (CATCHUP_LIMIT) * 60) {
			run_job(0);
		} else {
			syslog(LOG_NOTICE, "Job '%s' had to be skipped because it was too far in the past. (Was the system time set forward?)", jobs[0].command);
		}

		update_job(0, now);
		heapify_jobs(0);
	}
}
