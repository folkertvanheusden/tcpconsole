#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>

void error_exit(char *format, ...)
{
	char buffer[4096];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);

	fprintf(stderr, "%s: errno=%d (if applicable) -> %s\n", buffer, errno, strerror(errno));
	syslog(LOG_ERR, "'%s': %m", buffer);

	exit(127);
}
