#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <systemd/sd-journal.h>
#include <time.h>
#include <varlink.h>

#include "io.systemd.journal.varlink.h"
#include "util.h"

enum {
        ERROR_PANIC = 1,
        ERROR_MISSING_ADDRESS,

        ERROR_MAX
};

static const char *error_strings[] = {
        [ERROR_PANIC]           = "Panic",
        [ERROR_MISSING_ADDRESS] = "MissingAddress"
};

typedef struct {
        VarlinkCall *call;

        struct sd_journal *journal;
        char *cursor;

        /* the epoll that contains the journal's fd */
        int epoll_fd;
} Monitor;

static const char *priorities[] = {
        "debug",
        "information",
        "notice",
        "warning",
        "error",
        "alert",
        "critical",
        "emergency"
};

static long exit_error(long error) {
        fprintf(stderr, "Error: %s\n", error_strings[error]);

        return error;
}

static long epoll_add(int epoll_fd, int fd, void *ptr) {
        struct epoll_event event = {
                .events = EPOLLIN,
                .data = {
                        .ptr = ptr
                }
        };

        return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

static long journal_get_string(sd_journal *journal, const char *field, char **stringp) {
        const void *data;
        unsigned long field_length;
        unsigned long length;
        long r;

        r = sd_journal_get_data(journal, field, &data, &length);
        if (r < 0)
                return r;

        field_length = strlen(field) + 1; /* include the '=' */

        if (length < field_length)
                return -EBADMSG;

        *stringp = strndup(data + field_length, length - field_length);

        return 0;
}

static long journal_get_int(sd_journal *journal, const char *field, int64_t *numberp) {
        _cleanup_(freep) char *string = NULL;
        char *end;
        int64_t number;
        long r;

        r = journal_get_string(journal, field, &string);
        if (r < 0)
                return r;

        number = strtoll(string, &end, 10);
        if (end == string)
                return -EINVAL;

        *numberp = number;

        return 0;
}

static long format_time_rfc3339(uint64_t usec, char *string, unsigned long max) {
        time_t time;
        struct tm tm;

        time = usec / 1000000;

        if (!gmtime_r(&time, &tm))
                return -EINVAL;

        strftime(string, max, "%Y-%m-%d %H:%M:%SZ", &tm);

        return 0;
}


static void monitor_free(Monitor *monitor) {
        if (monitor->journal) {
                epoll_ctl(monitor->epoll_fd, EPOLL_CTL_DEL, sd_journal_get_fd(monitor->journal), NULL);
                sd_journal_close(monitor->journal);
        }

        varlink_call_unref(monitor->call);
        free(monitor->cursor);

        free(monitor);
}

static void monitor_freep(Monitor **monitorp) {
        if (*monitorp)
                monitor_free(*monitorp);
}

static void monitor_canceled(VarlinkCall *call, void *userdata) {
        Monitor *monitor = userdata;

        monitor_free(monitor);
}

static long monitor_new(Monitor **monitorp, VarlinkCall *call, int epoll_fd) {
        _cleanup_(monitor_freep) Monitor *monitor = NULL;
        long r;

        monitor = calloc(1, sizeof(Monitor));
        monitor->epoll_fd = epoll_fd;

        monitor->call = varlink_call_ref(call);
        varlink_call_set_canceled_callback(call, monitor_canceled, monitor);

        r = sd_journal_open(&monitor->journal, SD_JOURNAL_LOCAL_ONLY);
        if (r < 0)
                return r;

        if (epoll_add(epoll_fd, sd_journal_get_fd(monitor->journal), monitor) < 0)
                return -errno;

        r = sd_journal_seek_tail(monitor->journal);
        if (r < 0)
                return r;

        *monitorp = monitor;
        monitor = NULL;

        return 0;
}

static long journal_read_next_entry(sd_journal *journal, VarlinkObject **entryp) {
        _cleanup_(varlink_object_unrefp) VarlinkObject *entry = NULL;
        _cleanup_(freep) char *cursor = NULL;
        _cleanup_(freep) char *message = NULL;
        _cleanup_(freep) char *process = NULL;
        uint64_t usec;
        char timestr[50];
        int64_t priority;
        long r;

        r = sd_journal_next(journal);
        if (r <= 0)
                return r;

        r = sd_journal_get_cursor(journal, &cursor);
        if (r < 0)
                return r;

        r = sd_journal_get_realtime_usec(journal, &usec);
        if (r < 0)
                return r;

        r = format_time_rfc3339(usec, timestr, 50);
        if (r < 0)
                return r;

        r = journal_get_string(journal, "MESSAGE", &message);
        if (r < 0)
                return r;

        r = journal_get_int(journal, "PRIORITY", &priority);
        if (r < 0)
                return r;

        if (priority < 0 || priority > 7)
                return -EINVAL;


        varlink_object_new(&entry);
        varlink_object_set_string(entry, "cursor", cursor);
        varlink_object_set_string(entry, "time", timestr);
        varlink_object_set_string(entry, "message", message);
        varlink_object_set_string(entry, "priority", priorities[priority]);

        if (journal_get_string(journal, "SYSLOG_IDENTIFIER", &process) == 0)
                varlink_object_set_string(entry, "process", process);
        else if (journal_get_string(journal, "_COMM", &process) == 0)
                varlink_object_set_string(entry, "process", process);

        *entryp = entry;
        entry = NULL;

        return 1;
}

static long monitor_read_entries(Monitor *monitor, VarlinkArray **entriesp) {
        _cleanup_(varlink_array_unrefp) VarlinkArray *entries = NULL;
        long n_read = 0;
        long r;

        varlink_array_new(&entries);

        for (;;) {
                _cleanup_(varlink_object_unrefp) VarlinkObject *entry = NULL;

                r = journal_read_next_entry(monitor->journal, &entry);
                if (r < 0)
                        return r;

                if (r == 0)
                        break;

                varlink_array_append_object(entries, entry);
                n_read += 1;
        }

        free(monitor->cursor);
        monitor->cursor = NULL;

        if (n_read > 0) {
                r = sd_journal_get_cursor(monitor->journal, &monitor->cursor);
                if (r < 0)
                        return r;
        }

        *entriesp = entries;
        entries = NULL;

        return n_read;
}

static long monitor_dispatch(Monitor *monitor) {
        _cleanup_(varlink_array_unrefp) VarlinkArray *entries = NULL;
        _cleanup_(varlink_object_unrefp) VarlinkObject *reply = NULL;
        int event;
        long r;

        event = sd_journal_process(monitor->journal);
        if (event == SD_JOURNAL_INVALIDATE) {
                if (monitor->cursor)
                        r = sd_journal_seek_cursor(monitor->journal, monitor->cursor);
                else
                        r = sd_journal_seek_tail(monitor->journal);
                if (r < 0)
                        return r;

        } else if (event != SD_JOURNAL_APPEND) {
                return 0;
        }

        r = monitor_read_entries(monitor, &entries);
        if (r < 0)
                return r;

        if (r == 0)
                return 0;

        varlink_object_new(&reply);
        varlink_object_set_array(reply, "entries", entries);

        return varlink_call_reply(monitor->call, reply, VARLINK_REPLY_CONTINUES);
}

static long io_systemd_journal_monitor(VarlinkServer *server,
                                       VarlinkCall *call,
                                       VarlinkObject *parameters,
                                       uint64_t flags,
                                       void *userdata) {
        int epoll_fd = (int)(unsigned long)userdata;
        Monitor *monitor;
        _cleanup_(varlink_object_unrefp) VarlinkObject *reply = NULL;
        _cleanup_(varlink_array_unrefp) VarlinkArray *entries = NULL;
        int64_t initial_lines = 10;
        long r;

        /* no _cleanup_ on monitor, it ties itself to life of the call */
        r = monitor_new(&monitor, call, epoll_fd);
        if (r < 0)
                return r;

        varlink_object_get_int(parameters, "initial_lines", &initial_lines);
        if (initial_lines < 0)
                return varlink_call_reply_invalid_parameters(call, "initial_lines", NULL);

        r = sd_journal_previous_skip(monitor->journal, initial_lines + 1);
        if (r < 0)
                return r;

        r = monitor_read_entries(monitor, &entries);
        if (r < 0)
                return r;

        varlink_object_new(&reply);
        varlink_object_set_array(reply, "entries", entries);

        if (flags & VARLINK_CALL_MORE) {
                return varlink_call_reply(call, reply, VARLINK_REPLY_CONTINUES);
        } else {
                monitor_free(monitor);
                return varlink_call_reply(call, reply, 0);
        }
}

static int make_signalfd(void) {
        sigset_t mask;

        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

static long read_signal(int signal_fd) {
        struct signalfd_siginfo fdsi;
        long size;

        size = read(signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
        if (size != sizeof(struct signalfd_siginfo))
                return -EIO;

        return fdsi.ssi_signo;
}

int main(int argc, char **argv) {
        _cleanup_(varlink_server_freep) VarlinkServer *server = NULL;
        const char *address;
        _cleanup_(closep) int epoll_fd = -1;
        _cleanup_(closep) int signal_fd = -1;
        _cleanup_(varlink_object_unrefp) VarlinkObject *properties = NULL;
        int fd = -1;
        long r;

        if (!argv[1])
                return exit_error(ERROR_MISSING_ADDRESS);

        if (strcmp(argv[1], "--help") == 0) {
                printf("Usage: %s ADDRESS\n", program_invocation_short_name);
                printf("\n");
                printf("Provide a varlink service that exposes the systemd journal on ADDRESS\n");
                printf("\n");
                printf("  -h, --help      display this help text and exit\n");
                printf("\n");
                printf("Return values:\n");
                for (unsigned long i = 1; i < ERROR_MAX; i += 1)
                        printf(" %3lu %s\n", i, error_strings[i]);
                return EXIT_SUCCESS;
        }

        address = argv[1];

        /* An activator passed us our connection. */
        if (read(3, NULL, 0) == 0)
                fd = 3;

        r = varlink_object_new(&properties);
        if (r < 0)
                return exit_error(ERROR_PANIC);

        varlink_object_set_string(properties, "git", "https://github.com/varlink/io.systemd.devices");

        r = varlink_server_new(&server, address, fd,
                               "io.systemd.journal",
                               properties,
                               &io_systemd_journal_varlink, 1);
        if (r < 0)
                return exit_error(ERROR_PANIC);

        signal_fd = make_signalfd();
        if (signal_fd < 0)
                return exit_error(ERROR_PANIC);

        epoll_fd = epoll_create(EPOLL_CLOEXEC);
        if (epoll_fd < 0 ||
            epoll_add(epoll_fd, varlink_server_get_fd(server), server) < 0 ||
            epoll_add(epoll_fd, signal_fd, NULL) < 0)
                return exit_error(ERROR_PANIC);

        r = varlink_server_set_method_callback(server, "io.systemd.journal.Monitor",
                                               io_systemd_journal_monitor,
                                               (void *)(unsigned long)epoll_fd);
        if (r < 0)
                return exit_error(ERROR_PANIC);

        for (;;) {
                struct epoll_event event;
                int n;

                n = epoll_wait(epoll_fd, &event, 1, -1);
                if (n < 0) {
                        if (errno == EINTR)
                                continue;

                        return exit_error(ERROR_PANIC);
                }

                if (n == 0)
                        continue;

                if (event.data.ptr == server) {
                        r = varlink_server_process_events(server);
                        if (r < 0 && r != -EPIPE)
                                return exit_error(ERROR_PANIC);
                } else if (event.data.ptr == NULL) {
                        switch (read_signal(signal_fd)) {
                                case SIGTERM:
                                case SIGINT:
                                        return EXIT_SUCCESS;

                                default:
                                        return exit_error(ERROR_PANIC);
                        }
                } else {
                        Monitor *monitor = event.data.ptr;

                        r = monitor_dispatch(monitor);
                        if (r < 0 && r != -EPIPE)
                                return exit_error(ERROR_PANIC);
                }
        }

        return EXIT_SUCCESS;
}