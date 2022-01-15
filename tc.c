/* program must be invoked from /etc/inittab
 */
#include <errno.h>
#include <ctype.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdio.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <netinet/tcp.h>
#include <sys/klog.h>
#include <poll.h>

#include "error.h"

#define DEFAULT_LISTEN_PORT 4095

#define min(x, y) ((x) < (y) ? (x) : (y))

typedef struct
{
	int sysrq_fd;
	int vcsa0_fd;	/* virtual console 0 */

	char *dmesg_buffer;
	int dmesg_buffer_size;

} parameters_t;

int WRITE(int sock, const char *s, int len)
{
	while(len > 0)
	{
		int rc = write(sock, s, len);

		if (rc <= 0)
			return -1;

		len -= rc;
		s += rc;
	}

	return 0;
}

int sockprint(int fd, char *format, ...)
{
	char buffer[4096];
	int len;
	va_list ap;

	va_start(ap, format);
	len = vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);

	return WRITE(fd, buffer, len);
}

int readchar(int fd)
{
	for(;;)
	{
		char key;
		int rc = read(fd, &key, 1);

		if (rc <= 0)
			break;

		return key;
	}

	return -1;
}

int flush_socket(int fd)
{
	int rc = 0;

	for(;;) {
		char buffer[4096];
		struct pollfd fds[1] = { { fd, POLLIN, 0 } };
		rc = poll(fds, 1, 0);

		if (rc <= 0)
			break;

		rc = read(fd, buffer, sizeof(buffer));
		if (rc <= 0) {
			rc = -1;
			break;
		}
	}

	return rc;
}

char *get_string(int fd)
{
	static char buffer[4096];
	size_t len = 0;

	if (flush_socket(fd) == -1)
		return NULL;

	do
	{
		int key = readchar(fd);
		if (key == -1)
			return NULL;

		if (key == 0xff) { // flush telnet prot
			readchar(fd);
			readchar(fd);
			continue;
		}

		if (key == 10 || key == 13)
			break;

		buffer[len++] = key;
	}
	while(len < (sizeof(buffer) - 1));

	buffer[len] = 0x00;

	return buffer;
}

int ec_help(int fd)
{
	int rc = 0;

	rc |= sockprint(fd, "tcpconsole v " VERSION ", (C) 2009-2016 by folkert@vanheusden.com\r\n");
	rc |= sockprint(fd, "h: this help\r\n");
	rc |= sockprint(fd, "d: dump virtual console 0\r\n");
	rc |= sockprint(fd, "j: 'kill -9' for a given pid\r\n");
	rc |= sockprint(fd, "k: 'killall -9' for a given name\r\n");
	rc |= sockprint(fd, "l: dump dmesg\r\n");
	rc |= sockprint(fd, "m: dump dmesg & clear dmesg buffer\r\n");
	rc |= sockprint(fd, "p: process list\r\n");
	rc |= sockprint(fd, "i: show system (e.g. load)\r\n");
	rc |= sockprint(fd, "1-8: set dmesg loglevel\r\n");
	rc |= sockprint(fd, "q: log off\r\n");
	rc |= sockprint(fd, "\r\nSysreq:\r\n");
	rc |= sockprint(fd, "B - boot\r\n");
	rc |= sockprint(fd, "C - kexec\r\n");
	rc |= sockprint(fd, "D - list all locks\r\n");
	rc |= sockprint(fd, "E - SIGTERM to all but init, I - SIGKILL to all but init\r\n");
	rc |= sockprint(fd, "F - call oom_kill\r\n");
	rc |= sockprint(fd, "L - backtrace\r\n");
	rc |= sockprint(fd, "M - memory info dump, P - register/flags dump\r\n");
	rc |= sockprint(fd, "O - switch off\r\n");
	rc |= sockprint(fd, "Q - list hrtimers\r\n");
	rc |= sockprint(fd, "S - SYNC, U - umount\r\n");
	rc |= sockprint(fd, "T - tasklist dump, W - unint. tasks dump\r\n");

	return rc;
}

int sockerror(int fd, char *what)
{
	return sockprint(fd, "error on %s: %s (%d)\r\n", what, strerror(errno), errno);
}

int dump_virtual_console(int fd_out, int fd_in)
{
	struct{ char lines, cols, x, y; } scrn;
	int x, y;

	if (lseek(fd_in, 0, SEEK_SET) == -1)
		return sockerror(fd_out, "lseek");

	if (read(fd_in, &scrn, 4) == -1)
		return sockerror(fd_out, "read on vcs");

	for(y=0; y<scrn.lines; y++)
	{
		int nspaces = 0;

		for(x=0; x<scrn.cols; x++)
		{
			int loop;
			char ca[2];

			if (read(fd_in, ca, 2) == -1)
				return sockerror(fd_out, "read on vcs (data)");

			if (ca[0] != ' ')
			{
				for(loop=0; loop<nspaces; loop++)
					sockprint(fd_out, " ");
				nspaces = 0;

				sockprint(fd_out, "%c", ca[0]);
			}
			else
			{
				nspaces++;
			}
		}

		if (sockprint(fd_out, "\r\n") == -1)
			return -1;
	}

	return 0;
}

int dump_dmesg(int fd, char *dmesg_buffer, int dmesg_buffer_size, char clear)
{
	int loop, rc = 0;
	int nread = klogctl(clear?4:3, dmesg_buffer, dmesg_buffer_size);
	if (nread <= 0)
		return sockerror(fd, "klogctl(3)");

	for(loop=0; loop<nread; loop++) {
		if (dmesg_buffer[loop] == 10) {
			char cr = 13;
			rc = WRITE(fd, &cr, 1);
		}

		if (dmesg_buffer[loop] == 10 || (dmesg_buffer[loop] >= 32 && dmesg_buffer[loop] < 127))
			rc = WRITE(fd, &dmesg_buffer[loop], 1);

		if (rc)
			break;
	}

	return rc;
}

int set_dmesg_loglevel(int fd, int level)
{
	if (klogctl(8, NULL, level) == -1)
		return sockerror(fd, "klogctl(8)");

	return sockprint(fd, "dmesg loglevel set to %d\r\n", level);
}

int dump_loadavg(int fd)
{
	double avg[3];

	if (getloadavg(avg, 3) == -1)
		return sockerror(fd, "getloadavg(3)");

	return sockprint(fd, "load: 1min: %f, 5min: %f, 15min: %f\r\n", avg[0], avg[1], avg[2]);
}

int dump_ps(int fd)
{
	int rc = 0, tnprocs = 0, tnthreads = 0;
	struct dirent *de;
	DIR *dirp = opendir("/proc");

	while((de = readdir(dirp)) != NULL)
	{
		if (isdigit(de -> d_name[0]))
		{
			FILE *fh;
			static char path[300];

			snprintf(path, sizeof(path), "/proc/%s/stat", de -> d_name);
			fh = fopen(path, "r");
			if (fh)
			{
				static char fname[4096], dummystr[2];
				int dummy, nthreads, ppid, rss; 
				fscanf(fh, "%d %s %c %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &dummy, fname, &dummystr[0], &ppid, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &nthreads, &dummy, &dummy, &dummy, &rss);

				rc |= sockprint(fd, "%5s, ppid %5d, # threads: %2d, rss: %5d ", de -> d_name, ppid, nthreads, rss);

				tnprocs++;
				tnthreads += nthreads;
			}

			if (fh)
			{
				fclose(fh);

				snprintf(path, sizeof(path), "/proc/%s/cmdline", de -> d_name);
				fh = fopen(path, "r");
			}

			if (fh)
			{
				int len, loop;
				static char non_ret_buffer[4096];

				len = fread(non_ret_buffer, 1, sizeof(non_ret_buffer) - 1, fh);
				if (len < 0)
					rc |= sockerror(fd, "fread");
				else
				{
					non_ret_buffer[len] = 0x00;
					for(loop=0; loop<len; loop++)
					{
						if (non_ret_buffer[loop] == 0x00)
							non_ret_buffer[loop] = ' ';
					}
					rc |= sockprint(fd, "%d %s\r\n", len, non_ret_buffer);
				}

				fclose(fh);
			}
			else
			{
				rc |= sockprint(fd, "Error opening %s\r\n", path);
			}

			if (rc) break;
		}
	}

	closedir(dirp);

	rc |= sockprint(fd, "# procs: %d, # threads: %d\r\n", tnprocs, tnthreads);

	return rc;
}

int do_sysreq(int fd, char key, int sysreq_fd)
{
	int yn;

	if (key < 'a' || key > 'z')
		return sockprint(fd, "key out of range\r\n");

	if (sockprint(fd, "Send %c to sysreq? (y/n)\r\n", key) == -1)
		return -1;

	do
	{
		yn = readchar(fd);
	}
	while(yn != 'y' && yn != 'n' && yn != -1);

	if (yn == 'y')
	{
		if (WRITE(sysreq_fd, &key, 1) == -1)
			return sockerror(fd, "WRITE(sysreq_fd)");
	}

	return yn == -1 ? -1 : 0;
}

int kill_one_proc(int client_fd)
{
	int rc = 0;
	pid_t pid;
	char *entered;

	if (sockprint(client_fd, "Process id (PID, q to abort): ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(entered, "q") == 0)
		return 0;

	pid = atoi(entered);
	rc = sockprint(client_fd, "Killing pid %d\r\n", pid);

	if (kill(pid, SIGTERM) == -1)
		rc |= sockerror(client_fd, "kill(-9)");

	return rc;
}

int kill_procs(int client_fd)
{
	int nprocs = 0;
	struct dirent *de;
	DIR *dirp = opendir("/proc");
	char *entered;

	if (sockprint(client_fd, "Process name (q to abort): ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(entered, "q") == 0)
		return 0;

	if (sockprint(client_fd, "\r\nKilling proces %s\r\n", entered) == -1)
		return -1;

	while((de = readdir(dirp)) != NULL)
	{
		if (isdigit(de -> d_name[0]))
		{
			FILE *fh;
			static char path[300];

			snprintf(path, sizeof(path), "/proc/%s/stat", de -> d_name);
			fh = fopen(path, "r");
			if (fh)
			{
				static char fname[4096], dummystr[2], *pdummy;
				int dummy;
				fscanf(fh, "%d %s %c %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &dummy, fname, &dummystr[0], &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy);

				pdummy = strrchr(fname, ')');
				if (pdummy) *pdummy = 0x00;
				pdummy = fname + 1;
				if (strcmp(pdummy, entered) == 0)
				{
					pid_t pid = atoi(de -> d_name);
					if (sockprint(client_fd, "Killing pid %d\r\n", pid) == -1)
						break;

					if (kill(pid, SIGTERM) == -1)
					{
						if (sockerror(client_fd, "kill(-9)") == -1)
							break;
					}

					nprocs++;
				}

				fclose(fh);
			}
		}
	}

	closedir(dirp);

	return sockprint(client_fd, "Terminated %d processes\r\n", nprocs);
}

void serve_client(int fd, parameters_t *pars)
{
	sockprint(fd, "Enter 'h' for help\r\n");

	for(;;)
	{
		int key;

		if (sockprint(fd, "emergency console > ") == -1)
			break;

		if ((key = readchar(fd)) == -1)
			break;

		if (key < 32 || key > 126)
		{
			if (sockprint(fd, "\r") == -1)
				break;
			continue;
		}

		if (sockprint(fd, "%c\r\n", key) == -1)
			break;

		switch(key)
		{
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
				if (set_dmesg_loglevel(fd, key - '0') == -1)
					return;
				break;

			case 'd':
				if (dump_virtual_console(fd, pars -> vcsa0_fd) == -1)
					return;
				break;

			case '?':
			case 'h':
				if (ec_help(fd) == -1)
					return;
				break;

			case 'i':
				if (dump_loadavg(fd) == -1)
					return;
				break;

			case 'j':
				if (kill_one_proc(fd) == -1)
					return;
				break;

			case 'k':
				if (kill_procs(fd) == -1)
					return;
				break;

			case 'l':
				if (dump_dmesg(fd, pars -> dmesg_buffer, pars -> dmesg_buffer_size, 0) == -1)
					return;
				break;

			case 'm':
				if (dump_dmesg(fd, pars -> dmesg_buffer, pars -> dmesg_buffer_size, 1) == -1)
					return;
				break;

			case 'p':
				if (dump_ps(fd) == -1)
					return;
				break;

			case 'q':
				return;

			case 10:
			case 13:
				break;
			default:
				if (isupper(key))
					do_sysreq(fd, tolower(key), pars -> sysrq_fd);
				else
					sockprint(fd, "'%c' is not understood\r\n", key);
				break;
		}
	}
}

int verify_password(int client_fd, char *password)
{
	char *entered = NULL;
        const char suppress_goahead[] = { 0xff, 0xfb, 0x03 };
        const char dont_linemode[] = { 0xff, 0xfe, 0x22 };
        const char dont_new_env[] = { 0xff, 0xfe, 0x27 };
        const char will_echo[] = { 0xff, 0xfb, 0x01 };
        const char dont_echo[] = { 0xff, 0xfe, 0x01 };

        WRITE(client_fd, suppress_goahead, sizeof suppress_goahead );
        WRITE(client_fd, dont_linemode, sizeof dont_linemode );
        WRITE(client_fd, dont_new_env, sizeof dont_new_env);
        WRITE(client_fd, will_echo, sizeof will_echo);
        WRITE(client_fd, dont_echo, sizeof dont_echo);

	if (sockprint(client_fd, "Password: ") == -1)
		return -1;

	entered = get_string(client_fd);
	if (!entered)
		return -1;

	if (strcmp(password, entered) == 0)
		return 0;

	sockprint(client_fd, "Invalid password");

	return -1;
}

void listen_on_socket(int port, parameters_t *pars, char *password)
{
	int server_fd;
	struct sockaddr_in server_addr;
	int on = 1, optlen, sec60 = 60;

	memset(&server_addr, 0x00, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	server_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
		error_exit("error creating socket");

	if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1)
		error_exit("bind() failed");

	if (listen(server_fd, SOMAXCONN))
		error_exit("listen(%d) failed", SOMAXCONN);

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1)
		error_exit("setsockopt(SO_REUSEADDR) failed");

	if (setsockopt(server_fd, IPPROTO_TCP, TCP_KEEPIDLE, &sec60, sizeof(sec60)) == -1)
		error_exit("setsockopt(TCP_KEEPIDLE) failed");

	if (setsockopt(server_fd, IPPROTO_TCP, TCP_KEEPINTVL, &sec60, sizeof(sec60)) == -1)
		error_exit("setsockopt(TCP_KEEPINTVL) failed");

	syslog(LOG_PID|LOG_DAEMON, "Listening on %d", port);

	for(;;)
	{
		struct sockaddr_in client_addr;
		socklen_t client_addr_size = sizeof(client_addr);
		int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_size);
		if (client_fd == -1)
		{
			if (errno == EINTR)
				continue;

			sleep(1);
			continue;
		}

		optlen = sizeof(on);
		if(setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &on, optlen) == -1)
		{
			if (sockerror(client_fd, "setsockopt(SO_KEEPALIVE)") == -1)
			{
				close(client_fd);
				continue;
			}
		}

		if (verify_password(client_fd, password) == 0)
			serve_client(client_fd, pars);

		close(client_fd);
	}
}

void write_pidfile(char *file)
{
	FILE *pidf = fopen(file, "w");
	if (pidf == NULL)
		error_exit("Error creating pid-file %s\n", file);

	fprintf(pidf, "%d\n", getpid());

	fclose(pidf);
}

int open_file(char *path, int mode)
{
	int fd = open(path, mode);
	if (fd == -1)
		error_exit("Open_file(%s) failed", path);

	return fd;
}

char * read_password(char *file)
{
	char buffer[128], *pw, *lf;
	struct stat buf;
	int fd = open_file(file, O_RDONLY), rc;

	if (fstat(fd, &buf) == -1)
		error_exit("fstat(%s) failed", file);

	rc = read(fd, buffer, sizeof(buffer) - 1);
	if (rc == -1)
		error_exit("error reading password");
	buffer[rc] = 0x00;

	lf = strchr(buffer, '\n');
	if (lf) *lf = 0x00;

	close(fd);

	pw = strdup(buffer);
	if (!pw)
		error_exit("strdup() failed");

	return pw;
}

int main(int argc, char *argv[])
{
	char *password;
	int port = DEFAULT_LISTEN_PORT;
	parameters_t pars;
	struct sched_param sched_par;

	openlog("tcpconsole", LOG_CONS|LOG_NDELAY|LOG_NOWAIT|LOG_PID, LOG_DAEMON);

	if (getuid())
		error_exit("This program must be invoked with root-rights.");

	password = read_password("/etc/tcpconsole.pw");

	if (signal(SIGTERM, SIG_IGN) == SIG_ERR)
		error_exit("signal(SIGTERM) failed");

	if (signal(SIGHUP,  SIG_IGN) == SIG_ERR)
		error_exit("signal(SIGHUP) failed");

	pars.sysrq_fd = open_file("/proc/sysrq-trigger", O_WRONLY);
	pars.vcsa0_fd = open_file("/dev/vcsa", O_RDONLY);

	if (setpriority(PRIO_PROCESS, 0, -10) == -1)
		error_exit("Setpriority failed");

	if (nice(-20) == -1)
		error_exit("Failed to set nice-value to -20");

	if (mlockall(MCL_CURRENT) == -1 || mlockall(MCL_FUTURE) == -1)
		error_exit("Failed to lock program in core");

	memset(&sched_par, 0x00, sizeof(sched_par));
	sched_par.sched_priority = sched_get_priority_max(SCHED_RR);
	if (sched_setscheduler(0, SCHED_RR, &sched_par) == -1)
		error_exit("Failed to set scheduler properties for this process");

	syslog(LOG_INFO, "tcpconsole started");

	write_pidfile("/var/run/tcpconsole.pid");

	if ((pars.dmesg_buffer_size = klogctl(10, NULL, 0)) == -1)
		error_exit("klogctl(10) failed");
	pars.dmesg_buffer = (char *)malloc(pars.dmesg_buffer_size + 1);
	if (!pars.dmesg_buffer)
		error_exit("malloc failure");

	listen_on_socket(port, &pars, password);

	return 1;
}
