/*
 * Testing framework for libgpiod.
 *
 * Copyright (C) 2017 Bartosz Golaszewski <bartekgola@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 */

#include "gpiod-test.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <libgen.h>
#include <libkmod.h>
#include <libudev.h>
#include <pthread.h>
#include <regex.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/signalfd.h>

#define NORETURN	__attribute__((noreturn))
#define MALLOC		__attribute__((malloc))

static const char mockup_devpath[] = "/devices/platform/gpio-mockup/gpiochip";

struct mockup_chip {
	char *path;
	char *name;
	unsigned int number;
};

struct event_thread {
	pthread_t thread_id;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool running;
	bool should_stop;
	unsigned int chip_index;
	unsigned int line_offset;
	unsigned int freq;
	int event_type;
};

struct gpiotool_proc {
	pid_t pid;
	bool running;
	int stdin_fd;
	int stdout_fd;
	int stderr_fd;
	char *stdout;
	char *stderr;
	int sig_fd;
	int exit_status;
};

struct test_context {
	struct mockup_chip **chips;
	size_t num_chips;
	bool test_failed;
	char *failed_msg;
	struct event_thread event;
	struct gpiotool_proc tool_proc;
	bool running;
};

static struct {
	struct _test_case *test_list_head;
	struct _test_case *test_list_tail;
	unsigned int num_tests;
	unsigned int tests_failed;
	struct kmod_ctx *module_ctx;
	struct kmod_module *module;
	struct test_context test_ctx;
	pid_t main_pid;
	char *progpath;
	int pipesize;
	char *pipebuf;
} globals;

enum {
	CNORM = 0,
	CGREEN,
	CRED,
	CREDBOLD,
	CYELLOW,
};

static const char *term_colors[] = {
	"\033[0m",
	"\033[32m",
	"\033[31m",
	"\033[1m\033[31m",
	"\033[33m",
};

static void set_color(int color)
{
	fprintf(stderr, "%s", term_colors[color]);
}

static void reset_color(void)
{
	fprintf(stderr, "%s", term_colors[CNORM]);
}

static void pr_raw_v(const char *fmt, va_list va)
{
	vfprintf(stderr, fmt, va);
}

static TEST_PRINTF(1, 2) void pr_raw(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	pr_raw_v(fmt, va);
	va_end(va);
}

static void print_header(const char *hdr, int color)
{
	char buf[9];

	snprintf(buf, sizeof(buf), "[%s]", hdr);

	set_color(color);
	pr_raw("%-8s", buf);
	reset_color();
}

static void vmsgn(const char *hdr, int hdr_clr, const char *fmt, va_list va)
{
	print_header(hdr, hdr_clr);
	pr_raw_v(fmt, va);
}

static void vmsg(const char *hdr, int hdr_clr, const char *fmt, va_list va)
{
	vmsgn(hdr, hdr_clr, fmt, va);
	pr_raw("\n");
}

static TEST_PRINTF(1, 2) void msg(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vmsg("INFO", CGREEN, fmt, va);
	va_end(va);
}

static TEST_PRINTF(1, 2) void err(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vmsg("ERROR", CRED, fmt, va);
	va_end(va);
}

static void die_test_cleanup(void)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;
	int status;

	if (proc->running) {
		kill(proc->pid, SIGKILL);
		waitpid(proc->pid, &status, 0);
	}

	if (globals.test_ctx.running)
		pr_raw("\n");
}

static TEST_PRINTF(1, 2) NORETURN void die(const char *fmt, ...)
{
	va_list va;

	die_test_cleanup();

	va_start(va, fmt);
	vmsg("FATAL", CRED, fmt, va);
	va_end(va);

	exit(EXIT_FAILURE);
}

static TEST_PRINTF(1, 2) NORETURN void die_perr(const char *fmt, ...)
{
	va_list va;

	die_test_cleanup();

	va_start(va, fmt);
	vmsgn("FATAL", CRED, fmt, va);
	pr_raw(": %s\n", strerror(errno));
	va_end(va);

	exit(EXIT_FAILURE);
}

static MALLOC void * xmalloc(size_t size)
{
	void *ptr;

	ptr = malloc(size);
	if (!ptr)
		die("out of memory");

	return ptr;
}

static MALLOC void * xzalloc(size_t size)
{
	void *ptr;

	ptr = xmalloc(size);
	memset(ptr, 0, size);

	return ptr;
}

static MALLOC void * xcalloc(size_t nmemb, size_t size)
{
	void *ptr;

	ptr = calloc(nmemb, size);
	if (!ptr)
		die("out of memory");

	return ptr;
}

static MALLOC char * xstrdup(const char *str)
{
	char *ret;

	ret = strdup(str);
	if (!ret)
		die("out of memory");

	return ret;
}

static MALLOC TEST_PRINTF(2, 3) char * xappend(char *str, const char *fmt, ...)
{
	char *new, *ret;
	va_list va;
	int status;

	va_start(va, fmt);
	status = vasprintf(&new, fmt, va);
	va_end(va);
	if (status < 0)
		die_perr("vasprintf");

	if (!str)
		return new;

	ret = realloc(str, strlen(str) + strlen(new) + 1);
	if (!ret)
		die("out of memory");

	strcat(ret, new);
	free(new);

	return ret;
}

static int get_pipesize(void)
{
	int pipe_fds[2], rv;

	rv = pipe(pipe_fds);
	if (rv < 0)
		die_perr("unable to create a pipe");

	/*
	 * Since linux v2.6.11 the default pipe capacity is 16 system pages.
	 * We make an assumption here that gpio-tools won't output more than
	 * that, so we can read everything after the program terminated. If
	 * they did output more than the pipe capacity however, the write()
	 * system call would block and the process would be killed by the
	 * testing framework.
	 */
	rv = fcntl(pipe_fds[0], F_GETPIPE_SZ);
	if (rv < 0)
		die_perr("unable to retrieve the pipe capacity");

	close(pipe_fds[0]);
	close(pipe_fds[1]);

	return rv;
}

static void check_chip_index(unsigned int index)
{
	if (index >= globals.test_ctx.num_chips)
		die("invalid chip number requested from test code");
}

static bool mockup_loaded(void)
{
	int state;

	if (!globals.module_ctx || !globals.module)
		return false;

	state = kmod_module_get_initstate(globals.module);

	return state == KMOD_MODULE_LIVE;
}

static void cleanup_func(void)
{
	/* Don't cleanup from child processes. */
	if (globals.main_pid != getpid())
		return;

	msg("cleaning up");

	free(globals.pipebuf);

	if (mockup_loaded())
		kmod_module_remove_module(globals.module, 0);

	if (globals.module)
		kmod_module_unref(globals.module);

	if (globals.module_ctx)
		kmod_unref(globals.module_ctx);
}

static void event_lock(void)
{
	pthread_mutex_lock(&globals.test_ctx.event.lock);
}

static void event_unlock(void)
{
	pthread_mutex_unlock(&globals.test_ctx.event.lock);
}

static void * event_worker(void *data TEST_UNUSED)
{
	struct event_thread *ev = &globals.test_ctx.event;
	struct timeval tv_now, tv_add, tv_res;
	struct timespec ts;
	int status, i, fd;
	char *path;
	ssize_t rd;
	char buf;

	for (i = 0;; i++) {
		event_lock();
		if (ev->should_stop) {
			event_unlock();
			break;
		}

		gettimeofday(&tv_now, NULL);
		tv_add.tv_sec = 0;
		tv_add.tv_usec = ev->freq * 1000;
		timeradd(&tv_now, &tv_add, &tv_res);
		ts.tv_sec = tv_res.tv_sec;
		ts.tv_nsec = tv_res.tv_usec * 1000;

		status = pthread_cond_timedwait(&ev->cond, &ev->lock, &ts);
		if (status == ETIMEDOUT) {
			path = xappend(NULL,
				       "/sys/kernel/debug/gpio-mockup-event/gpio-mockup-%c/%u",
				       'A' + ev->chip_index, ev->line_offset);

			fd = open(path, O_RDWR);
			free(path);
			if (fd < 0)
				die_perr("error opening gpio event file");

			if (ev->event_type == TEST_EVENT_RISING)
				buf = '1';
			else if (ev->event_type == TEST_EVENT_FALLING)
				buf = '0';
			else
				buf = i % 2 == 0 ? '1' : '0';

			rd = write(fd, &buf, 1);
			close(fd);
			if (rd < 0)
				die_perr("error writing to gpio event file");
			else if (rd != 1)
				die("invalid write size to gpio event file");
		} else if (status != 0) {
			die("error waiting for conditional variable: %s",
			    strerror(status));
		}

		event_unlock();
	}

	return NULL;
}

static void gpiotool_proc_make_pipes(int *in_fds, int *out_fds, int *err_fds)
{
	int status, i, *fds[3];

	fds[0] = in_fds;
	fds[1] = out_fds;
	fds[2] = err_fds;

	for (i = 0; i < 3; i++) {
		status = pipe(fds[i]);
		if (status < 0)
			die_perr("unable to create a pipe");
	}
}

static void gpiotool_proc_dup_fds(int in_fd, int out_fd, int err_fd)
{
	int old_fds[3], new_fds[3], i, status;

	old_fds[0] = in_fd;
	old_fds[1] = out_fd;
	old_fds[2] = err_fd;

	new_fds[0] = STDIN_FILENO;
	new_fds[1] = STDOUT_FILENO;
	new_fds[2] = STDERR_FILENO;

	for (i = 0; i < 3; i++) {
		status = dup2(old_fds[i], new_fds[i]);
		if (status < 0)
			die_perr("unable to duplicate a file descriptor");

		close(old_fds[i]);
	}
}

static char * gpiotool_proc_get_path(const char *tool)
{
	char *path, *progpath, *progdir;

	progpath = xstrdup(globals.progpath);
	progdir = dirname(progpath);
	path = xappend(NULL, "%s/../../src/tools/%s", progdir, tool);
	free(progpath);

	return path;
}

static NORETURN void gpiotool_proc_exec(const char *path, va_list va)
{
	size_t num_args;
	unsigned int i;
	char **argv;
	va_list va2;

	va_copy(va2, va);
	for (num_args = 2; va_arg(va2, char *) != NULL; num_args++);
	va_end(va2);

	argv = xcalloc(num_args, sizeof(char *));

	argv[0] = (char *)path;
	for (i = 1; i < num_args; i++)
		argv[i] = va_arg(va, char *);
	va_end(va);

	execv(path, argv);
	die_perr("unable to exec '%s'", path);
}

static void gpiotool_proc_cleanup(void)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;

	if (proc->stdout)
		free(proc->stdout);

	if (proc->stderr)
		free(proc->stderr);

	proc->stdout = proc->stderr = NULL;
}

void test_tool_signal(int signum)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;
	int rv;

	rv = kill(proc->pid, signum);
	if (rv)
		die_perr("unable to send signal to process %d", proc->pid);
}

void test_tool_stdin_write(const char *fmt, ...)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;
	ssize_t written;
	va_list va;
	char *in;
	int rv;

	va_start(va, fmt);
	rv = vasprintf(&in, fmt, va);
	va_end(va);
	if (rv < 0)
		die_perr("error building string");

	written = write(proc->stdin_fd, in, rv);
	free(in);
	if (written < 0)
		die_perr("error writing to child process' stdin");
	if (written != rv)
		die("unable to write all data to child process' stdin");
}

void test_tool_run(char *tool, ...)
{
	int in_fds[2], out_fds[2], err_fds[2], status;
	struct gpiotool_proc *proc;
	sigset_t sigmask;
	char *path;
	va_list va;

	proc = &globals.test_ctx.tool_proc;
	if (proc->running)
		die("unable to start %s - another tool already running", tool);

	if (proc->pid)
		gpiotool_proc_cleanup();

	event_lock();
	if (globals.test_ctx.event.running)
		die("refusing to fork when the event thread is running");

	gpiotool_proc_make_pipes(in_fds, out_fds, err_fds);
	path = gpiotool_proc_get_path(tool);

	status = access(path, R_OK | X_OK);
	if (status)
		die_perr("unable to execute '%s'", path);

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);

	status = sigprocmask(SIG_BLOCK, &sigmask, NULL);
	if (status)
		die_perr("unable to block SIGCHLD");

	proc->sig_fd = signalfd(-1, &sigmask, 0);
	if (proc->sig_fd < 0)
		die_perr("unable to create signalfd");

	proc->pid = fork();
	if (proc->pid < 0) {
		die_perr("unable to fork");
	} else if (proc->pid == 0) {
		/* Child process. */
		close(proc->sig_fd);
		gpiotool_proc_dup_fds(in_fds[0], out_fds[1], err_fds[1]);
		va_start(va, tool);
		gpiotool_proc_exec(path, va);
	} else {
		/* Parent process. */
		close(in_fds[0]);
		proc->stdin_fd = in_fds[1];
		close(out_fds[1]);
		proc->stdout_fd = out_fds[0];
		close(err_fds[1]);
		proc->stderr_fd = err_fds[0];

		proc->running = true;
	}

	event_unlock();
	free(path);
}

static void gpiotool_readall(int fd, char **out)
{
	ssize_t rd;
	int i;

	memset(globals.pipebuf, 0, globals.pipesize);
	rd = read(fd, globals.pipebuf, globals.pipesize);
	if (rd < 0) {
		die_perr("error reading output from subprocess");
	} else if (rd == 0) {
		*out = NULL;
	} else {
		for (i = 0; i < rd; i++) {
			if (!isascii(globals.pipebuf[i]))
				die("GPIO tool program printed a non-ASCII character");
		}

		*out = xzalloc(rd + 1);
		memcpy(*out, globals.pipebuf, rd);
	}
}

void test_tool_wait(void)
{
	struct signalfd_siginfo sinfo;
	struct gpiotool_proc *proc;
	struct pollfd pfd;
	sigset_t sigmask;
	int status;
	ssize_t rd;

	proc = &globals.test_ctx.tool_proc;

	if (!proc->running)
		die("gpiotool process not running");

	pfd.fd = proc->sig_fd;
	pfd.events = POLLIN | POLLPRI;

	status = poll(&pfd, 1, 5000);
	if (status == 0)
		die("tool program is taking too long to terminate");
	else if (status < 0)
		die_perr("error when polling the signalfd");

	rd = read(proc->sig_fd, &sinfo, sizeof(sinfo));
	close(proc->sig_fd);
	if (rd < 0)
		die_perr("error reading signal info");
	else if (rd != sizeof(sinfo))
		die("invalid size of signal info");

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);

	status = sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
	if (status)
		die_perr("unable to unblock SIGCHLD");

	gpiotool_readall(proc->stdout_fd, &proc->stdout);
	gpiotool_readall(proc->stderr_fd, &proc->stderr);

	waitpid(proc->pid, &proc->exit_status, 0);

	close(proc->stdin_fd);
	close(proc->stdout_fd);
	close(proc->stderr_fd);

	proc->running = false;
}

const char * test_tool_stdout(void)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;

	return proc->stdout;
}

const char * test_tool_stderr(void)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;

	return proc->stderr;
}

bool test_tool_exited(void)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;

	return WIFEXITED(proc->exit_status);
}

int test_tool_exit_status(void)
{
	struct gpiotool_proc *proc = &globals.test_ctx.tool_proc;

	return WEXITSTATUS(proc->exit_status);
}

static void check_kernel(void)
{
	int status, version, patchlevel;
	struct utsname un;

	msg("checking the linux kernel version");

	status = uname(&un);
	if (status)
		die_perr("uname");

	status = sscanf(un.release, "%d.%d", &version, &patchlevel);
	if (status != 2)
		die("error reading kernel release version");

	if (version < 4 || patchlevel < 11)
		die("linux kernel version must be at least v4.11 - got v%d.%d",
		    version, patchlevel);

	msg("kernel release is v%d.%d - ok to run tests", version, patchlevel);
}

static void check_gpio_mockup(void)
{
	const char *modpath;
	int status;

	msg("checking gpio-mockup availability");

	globals.module_ctx = kmod_new(NULL, NULL);
	if (!globals.module_ctx)
		die_perr("error creating kernel module context");

	status = kmod_module_new_from_name(globals.module_ctx,
					   "gpio-mockup", &globals.module);
	if (status)
		die_perr("error allocating module info");

	/* First see if we can find the module. */
	modpath = kmod_module_get_path(globals.module);
	if (!modpath)
		die("the gpio-mockup module does not exist in the system or is built into the kernel");

	/* Then see if we can freely load and unload it. */
	status = kmod_module_probe_insert_module(globals.module, 0,
						 "gpio_mockup_ranges=-1,4",
						 NULL, NULL, NULL);
	if (status)
		die_perr("unable to load gpio-mockup");

	status = kmod_module_remove_module(globals.module, 0);
	if (status)
		die_perr("unable to remove gpio-mockup");

	msg("gpio-mockup ok");
}

static void load_module(struct _test_chip_descr *descr)
{
	unsigned int i;
	char *modarg;
	int status;

	if (descr->num_chips == 0)
		return;

	modarg = xappend(NULL, "gpio_mockup_ranges=");
	for (i = 0; i < descr->num_chips; i++)
		modarg = xappend(modarg, "-1,%u,", descr->num_lines[i]);
	modarg[strlen(modarg) - 1] = '\0'; /* Remove the last comma. */

	if (descr->flags & TEST_FLAG_NAMED_LINES)
		modarg = xappend(modarg, " gpio_mockup_named_lines");

	status = kmod_module_probe_insert_module(globals.module, 0,
						 modarg, NULL, NULL, NULL);
	if (status)
		die_perr("unable to load gpio-mockup");

	free(modarg);
}

static int chipcmp(const void *c1, const void *c2)
{
	const struct mockup_chip *chip1 = *(const struct mockup_chip **)c1;
	const struct mockup_chip *chip2 = *(const struct mockup_chip **)c2;

	return chip1->number > chip2->number;
}

static bool devpath_is_mockup(const char *devpath)
{
	return !strncmp(devpath, mockup_devpath, sizeof(mockup_devpath) - 1);
}

static void prepare_test(struct _test_chip_descr *descr)
{
	const char *devpath, *devnode, *sysname;
	struct udev_monitor *monitor;
	unsigned int detected = 0;
	struct test_context *ctx;
	struct mockup_chip *chip;
	struct udev_device *dev;
	struct udev *udev_ctx;
	struct pollfd pfd;
	int status;

	ctx = &globals.test_ctx;
	memset(ctx, 0, sizeof(*ctx));
	ctx->num_chips = descr->num_chips;
	ctx->chips = xcalloc(ctx->num_chips, sizeof(*ctx->chips));
	pthread_mutex_init(&ctx->event.lock, NULL);
	pthread_cond_init(&ctx->event.cond, NULL);

	/*
	 * We'll setup the udev monitor, insert the module and wait for the
	 * mockup gpiochips to appear.
	 */

	udev_ctx = udev_new();
	if (!udev_ctx)
		die_perr("error creating udev context");

	monitor = udev_monitor_new_from_netlink(udev_ctx, "udev");
	if (!monitor)
		die_perr("error creating udev monitor");

	status = udev_monitor_filter_add_match_subsystem_devtype(monitor,
								 "gpio", NULL);
	if (status < 0)
		die_perr("error adding udev filters");

	status = udev_monitor_enable_receiving(monitor);
	if (status < 0)
		die_perr("error enabling udev event receiving");

	load_module(descr);

	pfd.fd = udev_monitor_get_fd(monitor);
	pfd.events = POLLIN | POLLPRI;

	while (ctx->num_chips > detected) {
		status = poll(&pfd, 1, 5000);
		if (status < 0)
			die_perr("error polling for uevents");
		else if (status == 0)
			die("timeout waiting for gpiochips");

		dev = udev_monitor_receive_device(monitor);
		if (!dev)
			die_perr("error reading device info");

		devpath = udev_device_get_devpath(dev);
		devnode = udev_device_get_devnode(dev);
		sysname = udev_device_get_sysname(dev);

		if (!devpath || !devnode || !sysname ||
		    !devpath_is_mockup(devpath)) {
			udev_device_unref(dev);
			continue;
		}

		chip = xzalloc(sizeof(*chip));
		chip->name = xstrdup(sysname);
		chip->path = xstrdup(devnode);
		status = sscanf(sysname, "gpiochip%u", &chip->number);
		if (status != 1)
			die("unable to determine chip number");

		ctx->chips[detected++] = chip;
		udev_device_unref(dev);
	}

	udev_monitor_unref(monitor);
	udev_unref(udev_ctx);

	/*
	 * We can't assume that the order in which the mockup gpiochip
	 * devices are created will be deterministic, yet we want the
	 * index passed to the test_chip_*() functions to correspond with the
	 * order in which the chips were defined in the TEST_DEFINE()
	 * macro.
	 *
	 * Once all gpiochips are there, sort them by chip number.
	 */
	qsort(ctx->chips, ctx->num_chips, sizeof(*ctx->chips), chipcmp);
}

static void run_test(struct _test_case *test)
{
	print_header("TEST", CYELLOW);
	pr_raw("'%s': ", test->name);

	globals.test_ctx.running = true;
	test->func();
	globals.test_ctx.running = false;

	if (globals.test_ctx.test_failed) {
		globals.tests_failed++;
		set_color(CREDBOLD);
		pr_raw("FAILED:");
		reset_color();
		set_color(CRED);
		pr_raw("\n\t\t'%s': %s\n",
		       test->name, globals.test_ctx.failed_msg);
		reset_color();
		free(globals.test_ctx.failed_msg);
	} else {
		set_color(CGREEN);
		pr_raw("PASS\n");
		reset_color();
	}
}

static void teardown_test(void)
{
	struct gpiotool_proc *tool_proc;
	struct mockup_chip *chip;
	struct event_thread *ev;
	unsigned int i;
	int status;

	event_lock();
	ev = &globals.test_ctx.event;

	if (ev->running) {
		ev->should_stop = true;
		pthread_cond_broadcast(&ev->cond);
		event_unlock();

		status = pthread_join(globals.test_ctx.event.thread_id, NULL);
		if (status != 0)
			die("error joining event thread: %s",
			    strerror(status));

		pthread_mutex_destroy(&globals.test_ctx.event.lock);
		pthread_cond_destroy(&globals.test_ctx.event.cond);
	} else {
		event_unlock();
	}

	tool_proc = &globals.test_ctx.tool_proc;
	if (tool_proc->running) {
		test_tool_signal(SIGKILL);
		test_tool_wait();
	}

	if (tool_proc->pid)
		gpiotool_proc_cleanup();

	for (i = 0; i < globals.test_ctx.num_chips; i++) {
		chip = globals.test_ctx.chips[i];

		free(chip->path);
		free(chip->name);
		free(chip);
	}

	free(globals.test_ctx.chips);

	if (mockup_loaded()) {
		status = kmod_module_remove_module(globals.module, 0);
		if (status)
			die_perr("unable to remove gpio-mockup");
	}
}

int main(int argc TEST_UNUSED, char **argv)
{
	struct _test_case *test;

	globals.main_pid = getpid();
	globals.progpath = argv[0];
	globals.pipesize = get_pipesize();
	globals.pipebuf = xmalloc(globals.pipesize);
	atexit(cleanup_func);

	msg("libgpiod test suite");
	msg("%u tests registered", globals.num_tests);

	check_kernel();
	check_gpio_mockup();

	msg("running tests");

	for (test = globals.test_list_head; test; test = test->_next) {
		prepare_test(&test->chip_descr);
		run_test(test);
		teardown_test();
	}

	if (!globals.tests_failed)
		msg("all tests passed");
	else
		err("%u out of %u tests failed",
		    globals.tests_failed, globals.num_tests);

	return globals.tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

void test_close_chip(struct gpiod_chip **chip)
{
	if (*chip)
		gpiod_chip_close(*chip);
}

void test_free_str(char **str)
{
	if (*str)
		free(*str);
}

void test_free_chip_iter(struct gpiod_chip_iter **iter)
{
	if (*iter)
		gpiod_chip_iter_free(*iter);
}

void test_free_chip_iter_noclose(struct gpiod_chip_iter **iter)
{
	if (*iter)
		gpiod_chip_iter_free_noclose(*iter);
}

const char * test_chip_path(unsigned int index)
{
	check_chip_index(index);

	return globals.test_ctx.chips[index]->path;
}

const char * test_chip_name(unsigned int index)
{
	check_chip_index(index);

	return globals.test_ctx.chips[index]->name;
}

unsigned int test_chip_num(unsigned int index)
{
	check_chip_index(index);

	return globals.test_ctx.chips[index]->number;
}

void _test_register(struct _test_case *test)
{
	struct _test_case *tmp;

	if (!globals.test_list_head) {
		globals.test_list_head = globals.test_list_tail = test;
		test->_next = NULL;
	} else {
		tmp = globals.test_list_tail;
		globals.test_list_tail = test;
		test->_next = NULL;
		tmp->_next = test;
	}

	globals.num_tests++;
}

void _test_print_failed(const char *fmt, ...)
{
	int status;
	va_list va;

	va_start(va, fmt);
	status = vasprintf(&globals.test_ctx.failed_msg, fmt, va);
	va_end(va);
	if (status < 0)
		die_perr("vasprintf");

	globals.test_ctx.test_failed = true;
}

void test_set_event(unsigned int chip_index, unsigned int line_offset,
		    int event_type, unsigned int freq)
{
	struct event_thread *ev = &globals.test_ctx.event;
	int status;

	event_lock();

	if (!ev->running) {
		status = pthread_create(&ev->thread_id, NULL,
					event_worker, NULL);
		if (status != 0)
			die("error creating event thread: %s",
			    strerror(status));

		ev->running = true;
	}

	ev->chip_index = chip_index;
	ev->line_offset = line_offset;
	ev->event_type = event_type;
	ev->freq = freq;

	pthread_cond_broadcast(&ev->cond);

	event_unlock();
}

bool test_regex_match(const char *str, const char *pattern)
{
	char errbuf[128];
	regex_t regex;
	bool ret;
	int rv;

	rv = regcomp(&regex, pattern, REG_EXTENDED | REG_NEWLINE);
	if (rv) {
		regerror(rv, &regex, errbuf, sizeof(errbuf));
		die("unable to compile regex '%s': %s", pattern, errbuf);
	}

	rv = regexec(&regex, str, 0, 0, 0);
	if (rv == REG_NOERROR) {
		ret = true;
	} else if (rv == REG_NOMATCH) {
		ret = false;
	} else {
		regerror(rv, &regex, errbuf, sizeof(errbuf));
		die("unable to run a regex match: %s", errbuf);
	}

	regfree(&regex);

	return ret;
}
