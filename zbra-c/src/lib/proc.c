/*
 * Child processes, invoked directly.
 *
 * No shell, anywhere. Every argument is passed as its own argv element, so a
 * package name like "foo; rm -rf /" is a package name that does not exist
 * rather than a second command.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proc.h"

static char *mkerr(const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;
    char   *out;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    out = strdup(buf);
    return out;
}

int zbra_have_program(const char *name)
{
    char        path[4096];
    size_t      i;
    size_t      n = 0;
    const char *env = getenv("PATH");

    if (name == NULL || *name == '\0')
        return 0;

    /* A name with a slash is used as given, like execv(2) does. */
    if (strchr(name, '/') != NULL)
        return access(name, X_OK) == 0;

    if (env != NULL) {
        const char *p = env;

        while (*p != '\0' && n < 16) {
            const char *end = strchr(p, ':');
            size_t      len = end != NULL ? (size_t)(end - p) : strlen(p);

            if (len > 0 && len < sizeof(path)) {
                memcpy(path, p, len);
                path[len] = '/';
                if (strlen(path) + strlen(name) + 1 < sizeof(path)) {
                    strcat(path, name);
                    if (access(path, X_OK) == 0)
                        return 1;
                }
            }

            if (end == NULL)
                break;
            p = end + 1;
        }
    }

    /* Fall back to the usual locations for a stripped-down environment. */
    {
        static const char *fallback[] = {
            "/usr/bin", "/bin", "/usr/sbin", "/sbin", "/usr/local/bin", NULL
        };

        for (i = 0; fallback[i] != NULL; i++) {
            snprintf(path, sizeof(path), "%s/%s", fallback[i], name);
            if (access(path, X_OK) == 0)
                return 1;
        }
    }

    return 0;
}

/* Read a whole pipe into a NUL-terminated buffer. */
static char *drain(int fd)
{
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = malloc(cap);

    if (buf == NULL)
        return NULL;

    for (;;) {
        ssize_t got;

        if (len + 1024 >= cap) {
            char *grown = realloc(buf, cap * 2);

            if (grown == NULL) {
                free(buf);
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }

        got = read(fd, buf + len, cap - len - 1);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return NULL;
        }
        if (got == 0)
            break;

        len += (size_t)got;
    }

    buf[len] = '\0';
    return buf;
}

/*
 * Run argv, optionally capturing stdout and/or stderr.
 *
 * The child's output is drained before waitpid() so a tool that writes more
 * than a pipe buffer cannot deadlock against a parent that is waiting for it
 * to exit.
 */
static int run_at(const char *dir, const char *stdin_path,
                  const char *const argv[], int capture_out, int capture_err,
                  char **out, char **errout, int *status)
{
    int   outfd[2] = { -1, -1 };
    int   errfd[2] = { -1, -1 };
    pid_t pid;
    int   wstatus = 0;
    char  reason[256];

    if (argv == NULL || argv[0] == NULL) {
        reason[0] = '\0';
        snprintf(reason, sizeof(reason), "no command given");
        if (errout != NULL)
            *errout = strdup(reason);
        return -1;
    }

    if (capture_out && pipe(outfd) != 0) {
        snprintf(reason, sizeof(reason), "cannot create a pipe: %s",
                 strerror(errno));
        if (errout != NULL)
            *errout = strdup(reason);
        return -1;
    }

    if (capture_err && pipe(errfd) != 0) {
        snprintf(reason, sizeof(reason), "cannot create a pipe: %s",
                 strerror(errno));
        if (errout != NULL)
            *errout = strdup(reason);
        if (outfd[0] >= 0) {
            close(outfd[0]);
            close(outfd[1]);
        }
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        snprintf(reason, sizeof(reason), "cannot fork: %s", strerror(errno));
        if (errout != NULL)
            *errout = strdup(reason);
        if (outfd[0] >= 0) {
            close(outfd[0]);
            close(outfd[1]);
        }
        if (errfd[0] >= 0) {
            close(errfd[0]);
            close(errfd[1]);
        }
        return -1;
    }

    if (pid == 0) {
        /*
         * Restore a default signal disposition: a caller that ignored SIGPIPE
         * or caught SIGINT should not pass that on to a child, or the tool
         * would misbehave in ways that look like zbra bugs.
         */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        if (dir != NULL && chdir(dir) != 0)
            _exit(126);

        if (stdin_path != NULL) {
            int fd = open(stdin_path, O_RDONLY);

            if (fd < 0)
                _exit(126);
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        if (outfd[1] >= 0) {
            close(outfd[0]);
            dup2(outfd[1], STDOUT_FILENO);
            close(outfd[1]);
        }
        if (errfd[1] >= 0) {
            close(errfd[0]);
            dup2(errfd[1], STDERR_FILENO);
            close(errfd[1]);
        }

        execvp(argv[0], (char *const *)(uintptr_t)argv);
        _exit(127);
    }

    if (outfd[1] >= 0)
        close(outfd[1]);
    if (errfd[1] >= 0)
        close(errfd[1]);

    if (capture_out)
        *out = drain(outfd[0]);
    if (capture_err)
        *errout = drain(errfd[0]);

    if (outfd[0] >= 0)
        close(outfd[0]);
    if (errfd[0] >= 0)
        close(errfd[0]);

    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR) {
            if (errout != NULL && *errout == NULL)
                *errout = mkerr("cannot wait for %s: %s", argv[0],
                                strerror(errno));
            return -1;
        }
    }

    if (status != NULL)
        *status = wstatus;

    return 0;
}

int zbra_run_to_file(const char *const argv[], const char *path, char **err,
                     int *status)
{
    int   fd;
    pid_t pid;
    int   wstatus = 0;

    if (argv == NULL || argv[0] == NULL || path == NULL) {
        if (err != NULL)
            *err = mkerr("no command given");
        return -1;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (err != NULL)
            *err = mkerr("cannot write %s: %s", path, strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(fd);
        if (err != NULL)
            *err = mkerr("cannot fork: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        dup2(fd, STDOUT_FILENO);
        close(fd);

        execvp(argv[0], (char *const *)(uintptr_t)argv);
        _exit(127);
    }

    close(fd);

    while (waitpid(pid, &wstatus, 0) < 0) {
        if (errno != EINTR) {
            if (err != NULL)
                *err = mkerr("cannot wait for %s: %s", argv[0],
                             strerror(errno));
            return -1;
        }
    }

    if (status != NULL)
        *status = wstatus;

    return 0;
}

int zbra_run_at(const char *dir, const char *stdin_path,
                const char *const argv[], char **out, char **err, int *status)
{
    return run_at(dir, stdin_path, argv, out != NULL, err != NULL, out, err,
                  status);
}

int zbra_run(const char *const argv[], char **out, int *status, char **err)
{
    return run_at(NULL, NULL, argv, out != NULL, 0, out, err, status);
}

int zbra_run_capture(const char *const argv[], char **out, char **err,
                     int *status)
{
    return run_at(NULL, NULL, argv, out != NULL, err != NULL, out, err,
                  status);
}