/*
 * Copyright (C) 2013 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <time.h>
#include <regex.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#define SYSLOG_NAMES
#include <syslog.h>

#include <libubox/ustream.h>
#include <libubox/blobmsg_json.h>
#include <libubox/usock.h>
#include <libubox/uloop.h>
#include "libubus.h"
#include "../rfc3339/timestamp.h"
#include "syslog.h"

enum {
	LOG_STDOUT,
	LOG_FILE,
	LOG_NET,
};

enum {
	LOG_MSG,
	LOG_ID,
	LOG_PRIO,
	LOG_SOURCE,
	LOG_TIME,
	__LOG_MAX
};

static const struct blobmsg_policy log_policy[] = {
	[LOG_MSG] = { .name = "msg", .type = BLOBMSG_TYPE_STRING },
	[LOG_ID] = { .name = "id", .type = BLOBMSG_TYPE_INT32 },
	[LOG_PRIO] = { .name = "priority", .type = BLOBMSG_TYPE_INT32 },
	[LOG_SOURCE] = { .name = "source", .type = BLOBMSG_TYPE_INT32 },
	[LOG_TIME] = { .name = "time", .type = BLOBMSG_TYPE_INT64 },
};

enum {
	TPL_FIELD_MESSAGE,
	TPL_FIELD_PRIORITY,
	TPL_FIELD_SOURCE,
	TPL_FIELD_TIMESTAMP,
	TPL_FIELD_RFC3339,
};

static const char *TPL_FIELDS[] = {
	[TPL_FIELD_MESSAGE] = "%message%",
	[TPL_FIELD_PRIORITY] = "%priority%",
	[TPL_FIELD_SOURCE] = "%source%",
	[TPL_FIELD_TIMESTAMP] = "%timestamp%",
	[TPL_FIELD_RFC3339] = "%rfc3339%",
};

static struct uloop_timeout retry;
static struct uloop_fd sender;
static regex_t regexp_preg;
static const char *log_file, *log_ip, *log_port, *log_prefix, *pid_file, *hostname, *regexp_pattern;
static const char *log_template;
static int log_type = LOG_STDOUT;
static int log_size, log_udp, log_follow, log_trailer_null = 0;
static int log_timestamp;

static const char* getcodetext(int value, CODE *codetable) {
	CODE *i;

	if (value >= 0)
		for (i = codetable; i->c_val != -1; i++)
			if (i->c_val == value)
				return (i->c_name);
	return "<unknown>";
};

static void log_handle_reconnect(struct uloop_timeout *timeout)
{
	sender.fd = usock((log_udp) ? (USOCK_UDP) : (USOCK_TCP), log_ip, log_port);
	if (sender.fd < 0) {
		fprintf(stderr, "failed to connect: %s\n", strerror(errno));
		uloop_timeout_set(&retry, 1000);
	} else {
		uloop_fd_add(&sender, ULOOP_READ);
		syslog(LOG_INFO, "Logread connected to %s:%s\n", log_ip, log_port);
	}
}

static void log_handle_fd(struct uloop_fd *u, unsigned int events)
{
	if (u->eof) {
		uloop_fd_delete(u);
		close(sender.fd);
		sender.fd = -1;
		uloop_timeout_set(&retry, 1000);
	}
}

static int log_notify(struct blob_attr *msg)
{
	struct blob_attr *tb[__LOG_MAX];
	struct stat s;
	char buf[512];
	char buf_rfc3339[sizeof "YYYY-MM-DDThh:mm:ss.xxxZ"];
	char buf_ts[32];
	char buf_p[11];
	uint32_t p;
	char *str;
	time_t t;
	uint32_t t_ms = 0;
	timestamp_t ts = { 0 };
	char *c, *m;
	int ret = 0;

	if (sender.fd < 0)
		return 0;

	blobmsg_parse(log_policy, ARRAY_SIZE(log_policy), tb, blob_data(msg), blob_len(msg));
	if (!tb[LOG_ID] || !tb[LOG_PRIO] || !tb[LOG_SOURCE] || !tb[LOG_TIME] || !tb[LOG_MSG])
		return 1;

	if ((log_type == LOG_FILE) && log_size && (!stat(log_file, &s)) && (s.st_size > log_size)) {
		char *old = malloc(strlen(log_file) + 5);

		close(sender.fd);
		if (old) {
			sprintf(old, "%s.old", log_file);
			rename(log_file, old);
			free(old);
		}
		sender.fd = open(log_file, O_CREAT | O_WRONLY | O_APPEND, 0600);
		if (sender.fd < 0) {
			fprintf(stderr, "failed to open %s: %s\n", log_file, strerror(errno));
			exit(-1);
		}
	}

	m = blobmsg_get_string(tb[LOG_MSG]);
	if (regexp_pattern &&
	    regexec(&regexp_preg, m, 0, NULL, 0) == REG_NOMATCH)
		return 0;
	t = blobmsg_get_u64(tb[LOG_TIME]) / 1000;
	t_ms = blobmsg_get_u64(tb[LOG_TIME]) % 1000;
	ts.sec = (int64_t) t;
	ts.nsec = t_ms * 1000000;
	timestamp_format_precision(buf_rfc3339, sizeof buf_rfc3339, &ts, 3);
	snprintf(buf_ts, sizeof(buf_ts), "[%lu.%03u] ", (unsigned long) t, t_ms);
	c = ctime(&t);
	p = blobmsg_get_u32(tb[LOG_PRIO]);
	c[strlen(c) - 1] = '\0';
	str = blobmsg_format_json(msg, true);
	snprintf(buf_p, sizeof buf_p, "%u", p);

	if (log_template) {
		size_t len;
		int tpli = -1;

		if ((len = strlen(log_template) + 1) > sizeof buf) {
			fprintf(stderr, "size of template is larger than the internal buffer\n");
			return 1;
		}
		strncpy(buf, log_template, len);

		char *substr = buf;
		for (;;) {
			char *substrnext = NULL;
			for (unsigned short i = 0; i < sizeof TPL_FIELDS / sizeof (char *); ++i) {
				char *substrbuf = strstr(substr, TPL_FIELDS[i]);
				if (!substrbuf || (substrnext && substrbuf > substrnext))
					continue;
				substrnext = substrbuf;
				tpli = i;
			}
			if (!substrnext)
				break;

			substr = substrnext;
			char *field = NULL;
			switch (tpli) {
			case TPL_FIELD_MESSAGE:
				field = m;
				break;
			case TPL_FIELD_PRIORITY:
				field = buf_p;
				break;
			case TPL_FIELD_SOURCE:
				switch (blobmsg_get_u32(tb[LOG_SOURCE])) {
				case SOURCE_KLOG:
					field = "kernel";
					break;
				case SOURCE_SYSLOG:
					field = "syslog";
					break;
				case SOURCE_INTERNAL:
					field = "internal";
					break;
				default:
					field = "-";
					break;
				}
				break;
			case TPL_FIELD_TIMESTAMP:
				field = buf_ts;
				break;
			case TPL_FIELD_RFC3339:
				field = buf_rfc3339;
				break;
			}
			if (!field || tpli < 0)
				continue;

			char buf2[sizeof buf] = {'\0'};
			short availen = sizeof buf2 - 1;

			availen -= substr - buf;
			strncat(buf2, buf, substr - buf);

			size_t fieldlen = strlen(field);
			availen -= fieldlen;
			if (availen < 0) {
				fprintf(stderr, "size of log is larger than the internal buffer\n");
				return 1;
			}
			strncat(buf2, field, fieldlen);

			size_t fieldkwlen = strlen(TPL_FIELDS[tpli]);
			len = strlen(substr) - fieldkwlen;
			availen -= len;
			if (availen < 0) {
				fprintf(stderr, "size of log is larger than the internal buffer\n");
				return 1;
			}
			strncat(buf2, substr + fieldkwlen, len);

			strncpy(buf, buf2, sizeof buf2 - availen);
			substr += fieldlen;
		}
	}

	if (log_type == LOG_NET) {
		int err;

		if (!log_template) {
			snprintf(buf, sizeof(buf), "<%u>", p);
			strncat(buf, c + 4, sizeof buf_p);
			if (log_timestamp) {
				strncat(buf, buf_ts, sizeof(buf) - strlen(buf) - 1);
			}
			if (hostname) {
				strncat(buf, hostname, sizeof(buf) - strlen(buf) - 1);
				strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
			}
			if (log_prefix) {
				strncat(buf, log_prefix, sizeof(buf) - strlen(buf) - 1);
				strncat(buf, ": ", sizeof(buf) - strlen(buf) - 1);
			}
			if (blobmsg_get_u32(tb[LOG_SOURCE]) == SOURCE_KLOG)
				strncat(buf, "kernel: ", sizeof(buf) - strlen(buf) - 1);
			strncat(buf, m, sizeof(buf) - strlen(buf) - 1);
		}
		if (log_udp)
			err = write(sender.fd, buf, strlen(buf));
		else {
			size_t buflen = strlen(buf);
			if (!log_trailer_null)
				buf[buflen] = '\n';
			err = send(sender.fd, buf, buflen + 1, 0);
		}

		if (err < 0) {
			syslog(LOG_INFO, "failed to send log data to %s:%s via %s\n",
				log_ip, log_port, (log_udp) ? ("udp") : ("tcp"));
			uloop_fd_delete(&sender);
			close(sender.fd);
			sender.fd = -1;
			uloop_timeout_set(&retry, 1000);
		}
	} else {
		if (!log_template) {
			snprintf(buf, sizeof(buf), "%s %s%s.%s%s %s\n",
				c, log_timestamp ? buf_ts : "",
				getcodetext(LOG_FAC(p) << 3, facilitynames),
				getcodetext(LOG_PRI(p), prioritynames),
				(blobmsg_get_u32(tb[LOG_SOURCE])) ? ("") : (" kernel:"), m);
		} else {
			size_t buflen = strlen(buf);
			buflen = (buflen < sizeof buf - 1) ? buflen : (sizeof buf - 2);
			buf[buflen++] = '\n';
			buf[buflen] = '\0';
		}
		ret = write(sender.fd, buf, strlen(buf));
	}

	free(str);
	if (log_type == LOG_FILE)
		fsync(sender.fd);

	return ret;
}

static int usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n"
		"Options:\n"
		"    -s <path>		Path to ubus socket\n"
		"    -l	<count>		Got only the last 'count' messages\n"
		"    -e	<pattern>	Filter messages with a regexp\n"
		"    -r	<server> <port>	Stream message to a server\n"
		"    -F	<file>		Log file\n"
		"    -S	<bytes>		Log size\n"
		"    -p	<file>		PID file\n"
		"    -h	<hostname>	Add hostname to the message\n"
		"    -P	<prefix>	Prefix custom text to streamed messages\n"
		"    -T	<template>	Custom log output template\n"
		"    -f			Follow log messages\n"
		"    -u			Use UDP as the protocol\n"
		"    -t			Add an extra timestamp\n"
		"    -0			Use \\0 instead of \\n as trailer when using TCP\n"
		"\n", prog);
	return 1;
}

static void logread_fd_data_cb(struct ustream *s, int bytes)
{
	while (true) {
		struct blob_attr *a;
		int len, cur_len;

		a = (void*) ustream_get_read_buf(s, &len);
		if (len < sizeof(*a))
			break;

		cur_len = blob_len(a) + sizeof(*a);
		if (len < cur_len)
			break;

		log_notify(a);
		ustream_consume(s, cur_len);
	}
	if (!log_follow)
		uloop_end();
}

static void logread_fd_cb(struct ubus_request *req, int fd)
{
	static struct ustream_fd test_fd;

	test_fd.stream.notify_read = logread_fd_data_cb;
	ustream_fd_init(&test_fd, fd);
}

int main(int argc, char **argv)
{
	static struct ubus_request req;
	struct ubus_context *ctx;
	uint32_t id;
	const char *ubus_socket = NULL;
	int ch, ret, lines = 0;
	static struct blob_buf b;
	int tries = 5;

	signal(SIGPIPE, SIG_IGN);

	while ((ch = getopt(argc, argv, "u0fcs:l:r:F:p:S:P:h:e:tT:")) != -1) {
		switch (ch) {
		case 'u':
			log_udp = 1;
			break;
		case '0':
			log_trailer_null = 1;
			break;
		case 's':
			ubus_socket = optarg;
			break;
		case 'r':
			log_ip = optarg++;
			log_port = argv[optind++];
			break;
		case 'F':
			log_file = optarg;
			break;
		case 'p':
			pid_file = optarg;
			break;
		case 'P':
			log_prefix = optarg;
			break;
		case 'f':
			log_follow = 1;
			break;
		case 'l':
			lines = atoi(optarg);
			break;
		case 'S':
			log_size = atoi(optarg);
			if (log_size < 1)
				log_size = 1;
			log_size *= 1024;
			break;
		case 'h':
			hostname = optarg;
			break;
		case 'e':
			if (!regcomp(&regexp_preg, optarg, REG_NOSUB)) {
				regexp_pattern = optarg;
			}
			break;
		case 't':
			log_timestamp = 1;
			break;
		case 'T':
			log_template = optarg;
			break;
		default:
			return usage(*argv);
		}
	}
	uloop_init();

	ctx = ubus_connect(ubus_socket);
	if (!ctx) {
		fprintf(stderr, "Failed to connect to ubus\n");
		return -1;
	}
	ubus_add_uloop(ctx);

	/* ugly ugly ugly ... we need a real reconnect logic */
	do {
		ret = ubus_lookup_id(ctx, "log", &id);
		if (ret) {
			fprintf(stderr, "Failed to find log object: %s\n", ubus_strerror(ret));
			sleep(1);
			continue;
		}

		blob_buf_init(&b, 0);
		blobmsg_add_u8(&b, "stream", 1);
		if (lines)
			blobmsg_add_u32(&b, "lines", lines);
		else if (log_follow)
			blobmsg_add_u32(&b, "lines", 0);
		if (log_follow) {
			if (pid_file) {
				FILE *fp = fopen(pid_file, "w+");
				if (fp) {
					fprintf(fp, "%d", getpid());
					fclose(fp);
				}
			}
		}

		if (log_ip && log_port) {
			openlog("logread", LOG_PID, LOG_DAEMON);
			log_type = LOG_NET;
			sender.cb = log_handle_fd;
			retry.cb = log_handle_reconnect;
			uloop_timeout_set(&retry, 1000);
		} else if (log_file) {
			log_type = LOG_FILE;
			sender.fd = open(log_file, O_CREAT | O_WRONLY| O_APPEND, 0600);
			if (sender.fd < 0) {
				fprintf(stderr, "failed to open %s: %s\n", log_file, strerror(errno));
				exit(-1);
			}
		} else {
			sender.fd = STDOUT_FILENO;
		}

		ubus_invoke_async(ctx, id, "read", b.head, &req);
		req.fd_cb = logread_fd_cb;
		ubus_complete_request_async(ctx, &req);

		uloop_run();
		ubus_free(ctx);
		uloop_done();

	} while (ret && tries--);

	return ret;
}
