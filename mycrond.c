#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <time.h>

#include <stdio.h>

#define CRONTAB "crontab"

struct Job
{
	long long minutes;
	long hours;
	long mdays;
	short months;
	/* short wdays; */
};

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	exit(1);
}

/*static char *
read_file(const char *filename)
{
	struct stat info;
	char *text;
	ssize_t off = 0, ret;
	int fd;

	fd = open(filename, O_RDONLY);
	if (fd < 0) die("Can't open crontab:");

	if (fstat(fd, &info) < 0) die("Can't stat crontab:");

	text = malloc(info.st_size + 1);
	if (text == NULL) die("Can't allocate enough memory.");
	text[info.st_size] = 0;

	while (off < info.st_size) {
		ret = read(fd, text + off, info.st_size - off);
		if (ret < 0) die("Can't read crontab:");
		off += ret;
	}

	close(fd);

	return text;
}

static void
skip_space(char **line)
{
	char *cur = *line;
	while (*cur == ' ' || *cur == '\t' || *cur == '\r') ++cur;
	*line = cur;
}

static unsigned int
parse_num(char **str)
{
	unsigned int num = 0;
	char *cur = *str;
	while (*cur >= '0' && *cur <= '9') {
		num = num * 10 + *cur - '0';
		++cur;
	}
	*str = cur;
	return num;
}

static void
parse_table(const char *filename)
{
	char *text, *ptr, *eol;
	int lineno = 1;

	text = read_file(filename);
	ptr = text;
	for (;;) {
		eol = strchr(ptr, '\n');
		if (eol == NULL) break;
		*eol = 0;
		parse_line(ptr, lineno);
		ptr = eol + 1;
		++lineno;
	}
	parse_line(ptr, lineno);
	free(text);
}*/

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

static unsigned int
check_term(struct Job job, struct tm tm)
{
	unsigned int ret = 0;
	if (!(job.minutes >> tm.tm_min  & 1)) ret |= 1;
	if (!(job.hours   >> tm.tm_hour & 1)) ret |= 2;
	if (!(job.mdays   >> tm.tm_mday & 1)) ret |= 4;
	if (!(job.months  >> tm.tm_mon  & 1)) ret |= 8;
	return ret;
}

static void
next_term(struct Job job, struct tm *term)
{
	struct tm tm = *term;
	tm.tm_sec = 0;

	unsigned int init = check_term(job, tm);

	/* Determine minute, and exit early if possible. */
	if (!(init >> 1)) {
		++tm.tm_min;
		long long minutes_left = job.minutes & (~0LL << tm.tm_min);
		if (minutes_left != 0) {
			tm.tm_min = ffsll(minutes_left) - 1;
			goto finished;
		}
	}
	tm.tm_min = ffsll(job.minutes) - 1;

	/* Determine hour, and exit early if possible. */
	if (!(init >> 2)) {
		++tm.tm_hour;
		long hours_left = job.hours & ~0L << tm.tm_hour;
		if (hours_left != 0) {
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
	} while (check_term(job, tm) >> 2);

finished:
	*term = tm;
}

int
main()
{
	struct Job job = { 0 };
	job.minutes = (1 << 20);
	job.hours = (1 << 22);
	job.mdays = (1 << 29);
	job.months = (1 << 1) | (1 << 4) | (1 << 9);
	time_t now;
	time(&now);
	struct tm tm;
	localtime_r(&now, &tm);
	for (int i = 0; i < 100; ++i) {
		next_term(job, &tm);
		char buf[100];
		strftime(buf, sizeof(buf), "%M%t%H%t%d%t%b%t%a%t(%Y)", &tm);
		printf("%s\n", buf);
	}
	return 0;
}
