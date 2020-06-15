#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <time.h>
#include <syslog.h>

#include <stdio.h>

#define CRONTAB "crontab"
#define LOG_IDENT "crond"

#define MINUTES_MASK 0xFFFFFFFFFFFFFFLL
#define HOURS_MASK   0xFFFFFFL
#define MDAYS_MASK   0xFFFFFFFEL
#define MONTHS_MASK  0xFFF
#define WDAYS_MASK   0x7F

struct Job
{
	time_t time;
	long long minutes;
	long hours;
	long mdays;
	short months;
	short wdays;
	short lineno;
};

static int capJobs;
static int numJobs;
static struct Job *jobs;

static char *text;

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

/* (Gregorian) calendar awareness. */

static int
leap_year(int year)
{
	if (year % 4 != 0) return 0;
	if (year % 400 == 0) return 1;
	return year % 100 != 0;
}

static int
days_in_month(int month, int year)
{
	assert(month >= 0 && month < 12);
	if (month != 1) {
		return 30 + ((month % 7 + 1) & 1);
	} else {
		return 28 + leap_year(year);
	}
}

/* Job time-finding algorithm. */

static unsigned int
check_tm(struct Job job, struct tm tm)
{
	unsigned int ret = 0;
	if (!(job.minutes >> tm.tm_min  & 1)) ret |= 1;
	if (!(job.hours   >> tm.tm_hour & 1)) ret |= 2;
	if (!(job.mdays   >> tm.tm_mday & 1) &&
		!(job.wdays   >> tm.tm_wday & 1)) ret |= 4;
	if (!(job.months  >> tm.tm_mon  & 1)) ret |= 8;
	return ret;
}

static void
next_tm(struct Job job, struct tm *tm_ptr)
{
	struct tm tm = *tm_ptr;
	tm.tm_sec = 0;

	unsigned int init = check_tm(job, tm);

	/* Determine minute, and exit early if possible. */
	assert(job.minutes != 0);
	if (!(init >> 1)) {
		++tm.tm_min;
		long long minutes_left = job.minutes & ~0ULL << tm.tm_min;
		if (minutes_left != 0LL) {
			tm.tm_min = ffsll(minutes_left) - 1;
			goto finished;
		}
	}
	tm.tm_min = ffsll(job.minutes) - 1;

	/* Determine hour, and exit early if possible. */
	assert(job.hours != 0);
	if (!(init >> 2)) {
		++tm.tm_hour;
		long hours_left = job.hours & ~0UL << tm.tm_hour;
		if (hours_left != 0L) {
			tm.tm_hour = ffsl(hours_left) - 1;
			goto finished;
		}
	}
	tm.tm_hour = ffsl(job.hours) - 1;

	/* Determine day, month, and year. */
	do {
		++tm.tm_mday;
		if (tm.tm_mday > days_in_month(tm.tm_mon, 1900 + tm.tm_year)) {
			tm.tm_mday = 1;
			++tm.tm_mon;
			if (tm.tm_mon >= 12) {
				tm.tm_mon = 0;
				++tm.tm_year;
			}
		}
		tm.tm_wday = (tm.tm_wday + 1) % 7;
	} while (check_tm(job, tm) >> 2);

finished:
	*tm_ptr = tm;
}

/* Job queue operations. */

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

static void
update_job(int idx, time_t now)
{
	struct tm tm;
	localtime_r(&now, &tm);
	tm.tm_isdst = -1;
	next_tm(jobs[idx], &tm);
	jobs[idx].time = mktime(&tm);
}

static void
init_jobs(time_t now)
{
	int i;
	for (i = 0; i < numJobs; ++i) {
		update_job(i, now);
	}
	for (i = numJobs / 2 - 1; i >= 0; --i) {
		heapify_jobs(i);
	}
}

/* Crontab parsing. */

static char *
read_file(const char *filename)
{
	struct stat info;
	char *text;
	ssize_t off = 0, ret;
	int fd;

	fd = open(filename, O_RDONLY);
	if (fd < 0) die("Can't open crontab: %m");

	if (fstat(fd, &info) < 0) die("Can't stat crontab: %m");

	text = malloc(info.st_size + 1);
	if (text == NULL) die("Can't allocate enough memory.");
	text[info.st_size] = 0;

	while (off < info.st_size) {
		ret = read(fd, text + off, info.st_size - off);
		if (ret < 0) die("Can't read crontab: %m");
		off += ret;
	}

	close(fd);

	return text;
}

static int
skip_space(void)
{
	if (!(*text == ' ' || *text == '\t' || *text == '\r')) {
		return -1;
	}
	do {
		++text;
	} while (*text == ' ' || *text == '\t' || *text == '\r');
	return 0;
}

static int
parse_number(unsigned int *number)
{
	unsigned int num = 0;
	if (!(*text >= '0' && *text <= '9')) return -1;
	do {
		num = num * 10 + *text++ - '0';
	} while (*text >= '0' && *text <= '9');
	*number = num;
	return 0;
}

static int
parse_range(long long *field)
{
	if (*text == '*') {
		++text;
		return 0;
	}
	if (*text >= '0' && *text <= '9') {
		unsigned int first, last;
		if (parse_number(&first) < 0) return -1;

		if (*text == '-') {
			++text;
			if (parse_number(&last) < 0) return -1;
		} else {
			last = first;
		}

		if (first >= 64) return -1;
		if (last >= 64) return -1;
		for (unsigned int i = first; i <= last; ++i) {
			*field |= 1ULL << i;
		}
		return 0;
	}
	return -1;
}

static int
parse_field(long long *field)
{
	*field = 0LL;
	for (;;) {
		if (parse_range(field) < 0) return -1;
		if (*text != ',') break;
		++text;
	}
	return 0;
}

static void
parse_line(int lineno)
{
	struct Job job;
	long long field;

	memset(&job, 0, sizeof(job));
	job.lineno = lineno;

	/* We don't care if we actually find spaces here or not. */
	skip_space();
	
	/* Dismiss empty lines and comments. */
	if (*text == '#') return;
	if (*text == '\n' || *text == 0) return;
	
	if (parse_field(&field) < 0) goto bad_line;
	if ((field & MINUTES_MASK) != field) goto bad_line;
	job.minutes = field ? field : ~0LL;

	if (skip_space() < 0) goto bad_line;
	if (parse_field(&field) < 0) goto bad_line;
	if ((field & HOURS_MASK) != field) goto bad_line;
	job.hours = field ? field : ~0L;

	if (skip_space() < 0) goto bad_line;
	if (parse_field(&field) < 0) goto bad_line;
	if ((field & MDAYS_MASK) != field) goto bad_line;
	job.mdays = field;

	if (skip_space() < 0) goto bad_line;
	if (parse_field(&field) < 0) goto bad_line;
	if ((field & MONTHS_MASK) != field) goto bad_line;
	job.months = field ? field : ~0;

	if (skip_space() < 0) goto bad_line;
	if (parse_field(&field) < 0) goto bad_line;
	if ((field & WDAYS_MASK) != field) goto bad_line;
	job.wdays = field;

	if (!job.mdays && !job.wdays) {
		job.mdays = ~0L;
	}

	/* Add the job to the list and we're done. */
	if (numJobs >= capJobs) {
		capJobs *= 2;
		jobs = reallocarray(jobs, capJobs, sizeof(jobs[0]));
		if (jobs == NULL) die("Can't allocate memory for jobs.");
	}
	jobs[numJobs++] = job;
	return;

bad_line:
	syslog(LOG_WARNING, "Line %d will be ignored because of bad syntax.\n", lineno);
}

static void
parse_table(const char *filename)
{
	char *contents, *line, *eol;
	int lineno = 1;

	contents = read_file(filename);
	line = contents;
	for (;;) {
		text = line;
		parse_line(lineno);
		eol = strchr(line, '\n');
		if (eol == NULL) break;
		line = eol + 1;
		++lineno;
	}
	free(contents);
	text = NULL;
}

/* The main loop. */

int
main()
{
	openlog(LOG_IDENT, LOG_CONS, LOG_CRON);

	capJobs = 4;
	jobs = calloc(capJobs, sizeof(jobs[0]));
	if (jobs == NULL) die("Can't allocate memory for jobs.");

	parse_table(CRONTAB);

	if (numJobs == 0) {
		/* TODO lift this requirement by just permanently idling. */
		die("Must have at least one job.");
	}

	time_t now;
	time(&now);
	init_jobs(now);

	for (int t = 0; t < 20; ++t) {
		struct tm tm;
		localtime_r(&jobs[0].time, &tm);
		char buf[100];
		strftime(buf, sizeof(buf), "%M%t%H%t%d%t%b%t%a%t(%Y)", &tm);
		printf("%s\t(%d)\n", buf, jobs[0].lineno);

		update_job(0, jobs[0].time);
		heapify_jobs(0);
	}
	return 0;
}
