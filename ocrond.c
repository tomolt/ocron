#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <time.h>
#include <syslog.h>
#include <dirent.h>
#include <errno.h>

#include <stdio.h>

#define CRONTAB   "./crontab"
#define CRON_D    "./cron.d"
#define LOG_IDENT "crond"

#define MINUTES_MASK 0xFFFFFFFFFFFFFFFLL
#define HOURS_MASK   0xFFFFFFL
#define MDAYS_MASK   0xFFFFFFFEL
#define MONTHS_MASK  0xFFF
#define WDAYS_MASK   0xFF

#define IS_LEAP_YEAR(year) ((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 400 == 0))
#define VALID_HOUR(job, hour) ((job).hours >> (hour) & 1)
#define VALID_MDAY(job, mday) ((job).mdays >> (mday) & 1)
#define VALID_WDAY(job, wday) ((job).wdays >> (wday) & 1)
#define VALID_DAY(job, mday, wday) (VALID_MDAY(job, mday) || VALID_WDAY(job, wday))
#define VALID_MONTH(job, month) ((job).months >> (month) & 1)
#define VALID_DATE(job, mday, wday, month) (VALID_DAY(job, mday, wday) && VALID_MONTH(job, month))

static const char *no_aliases[] = { NULL };
static const char *months_aliases[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL
};
static const char *wdays_aliases[] = {
	"Sun", "Mon", "Tue", "Wed",
	"Thu", "Fri", "Sat", NULL
};

struct Job
{
	time_t time;
	long long minutes;
	char *command;
	long hours;
	long mdays;
	short months;
	short wdays;
};

static int capJobs;
static int numJobs;
static struct Job *jobs;

static char *text;
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

/* Job time-finding algorithm. */

static void
next_tm(struct Job job, struct tm *tm_ptr)
{
	struct tm tm = *tm_ptr;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;

	int today_alright = VALID_DATE(job, tm.tm_mday, tm.tm_wday, tm.tm_mon);

	/* Determine minute, and exit early if possible. */
	assert(job.minutes != 0);
	if (today_alright && VALID_HOUR(job, tm.tm_hour)) {
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
	if (today_alright) {
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
	} while (!VALID_DATE(job, tm.tm_mday, tm.tm_wday, tm.tm_mon));

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
	if (fd < 0) die("Can't open %s: %m", filename);

	if (fstat(fd, &info) < 0) die("Can't stat %s: %m", filename);

	text = malloc(info.st_size + 1);
	if (text == NULL) die("Can't allocate enough memory.");
	text[info.st_size] = 0;

	while (off < info.st_size) {
		ret = read(fd, text + off, info.st_size - off);
		if (ret < 0) die("Can't read %s: %m", filename);
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
parse_value(const char *aliases[], unsigned int *number)
{
	if (*text >= '0' && *text <= '9') {
		if (parse_number(number) < 0) return -1;
		return 0;
	}
	for (unsigned int i = 0; aliases[i] != NULL; ++i) {
		if (has_prefix(text, aliases[i])) {
			text += strlen(aliases[i]);
			*number = i;
			return 0;
		}
	}
	return -1;
}

static int
parse_range(const char *aliases[], long long *field)
{
	if (*text == '*') {
		++text;
		return 0;
	}

	unsigned int first, last, step = 1;
	if (parse_value(aliases, &first) < 0) return -1;
	last = first;

	if (*text == '-') {
		++text;
		if (parse_value(aliases, &last) < 0) return -1;

		if (*text == '/') {
			++text;
			if (parse_number(&step) < 0) return -1;
		}
	}

	if (first >= 64) return -1;
	if (last >= 64) return -1;
	if (step < 1) return -1;
	for (unsigned int i = first; i <= last; i += step) {
		*field |= 1ULL << i;
	}
	return 0;
}

static int
parse_field(const char *aliases[], long long *field)
{
	*field = 0LL;
	for (;;) {
		if (parse_range(aliases, field) < 0) return -1;
		if (*text != ',') break;
		++text;
	}
	return 0;
}

static int
parse_command(char **command)
{
	/* Guard against empty commands. */
	if (text >= eol) return -1;
	
	if ((*command = malloc(eol - text + 2)) == NULL) {
		die("Can't allocate enough memory.");
	}

	/* Decode escaped string into its raw form. */
	int esc = 0;
	char *out = *command;
	while (text < eol) {
		char c = *text++;
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
	char *delim = strchr(*command, '\n');
	if (delim == NULL) delim = out;
	*delim = 0;

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
	if (*text == '\n' || *text == 0) return 0;
	
	if (parse_field(no_aliases, &field) < 0) return -1;
	if ((field & MINUTES_MASK) != field) return -1;
	job.minutes = field ? field : ~0LL;

	if (skip_space() < 0) return -1;
	if (parse_field(no_aliases, &field) < 0) return -1;
	if ((field & HOURS_MASK) != field) return -1;
	job.hours = field ? field : ~0L;

	if (skip_space() < 0) return -1;
	if (parse_field(no_aliases, &field) < 0) return -1;
	if ((field & MDAYS_MASK) != field) return -1;
	job.mdays = field;

	if (skip_space() < 0) return -1;
	if (parse_field(months_aliases, &field) < 0) return -1;
	if ((field & MONTHS_MASK) != field) return -1;
	job.months = field ? field : ~0;

	if (skip_space() < 0) return -1;
	if (parse_field(wdays_aliases, &field) < 0) return -1;
	if ((field & WDAYS_MASK) != field) return -1;
	field |= field >> 7 & 1;
	job.wdays = field;

	if (!job.mdays && !job.wdays) {
		job.mdays = ~0L;
	}

	if (skip_space() < 0) return -1;
	if (parse_command(&job.command) < 0) return -1;

	/* Add the job to the list and we're done. */
	if (numJobs >= capJobs) {
		capJobs *= 2;
		jobs = reallocarray(jobs, capJobs, sizeof(jobs[0]));
		if (jobs == NULL) die("Can't allocate memory for jobs.");
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

static void
parse_everything(void)
{
	/* Yes, there's a race condition here, but all you can achieve with it
	 * is making ocrond exit on startup - there's easier ways to do that anyway. */
	if (!(access(CRONTAB, F_OK) < 0)) {
		parse_file(CRONTAB);
	}

	DIR *dir;
	if ((dir = opendir(CRON_D)) != NULL) {
		for (;;) {
			errno = 0;
			struct dirent *ent = readdir(dir);
			if (ent == NULL) {
				if (errno) die("Couldn't walk %s: %m", CRON_D);
				else break;
			}
			parse_file(ent->d_name);
		}
		closedir(dir);
	}
}

/* The main loop. */

int
main()
{
	openlog(LOG_IDENT, LOG_CONS, LOG_CRON);

	capJobs = 4;
	jobs = calloc(capJobs, sizeof(jobs[0]));
	if (jobs == NULL) die("Can't allocate memory for jobs.");

	parse_everything();

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
		printf("%s\t%s\n", buf, jobs[0].command);

		update_job(0, jobs[0].time);
		heapify_jobs(0);
	}
	return 0;
}
