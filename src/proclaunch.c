#include "proclaunch.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int append_win_arg(char **buf, size_t *len, size_t *cap, const char *arg) {
    int need_quotes = 0;
    if (arg[0] == '\0') {
        need_quotes = 1;
    } else {
        for (const char *p = arg; *p != '\0'; ++p) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                need_quotes = 1;
                break;
            }
        }
    }

    size_t extra = 4; /* пробел-разделитель + две кавычки + '\0' */
    if (!need_quotes) {
        extra += strlen(arg);
    } else {
        size_t bs = 0;
        for (const char *p = arg; *p != '\0'; ++p) {
            if (*p == '\\') {
                bs++;
            } else if (*p == '"') {
                extra += bs * 2 + 2;
                bs = 0;
            } else {
                extra += bs + 1;
                bs = 0;
            }
        }
        extra += bs * 2;
    }

    if (*len + extra >= *cap) {
        size_t new_cap = (*cap == 0) ? 256 : (*cap * 2);
        while (new_cap < *len + extra + 1) {
            new_cap *= 2;
        }
        char *grown = (char *)realloc(*buf, new_cap);
        if (!grown) {
            return -1;
        }
        *buf = grown;
        *cap = new_cap;
    }

    char *dst = *buf + *len;
    if (*len > 0) {
        *dst++ = ' ';
    }

    if (!need_quotes) {
        size_t n = strlen(arg);
        memcpy(dst, arg, n);
        dst += n;
    } else {
        *dst++ = '"';
        size_t bs = 0;
        for (const char *p = arg; *p != '\0'; ++p) {
            if (*p == '\\') {
                bs++;
            } else if (*p == '"') {
                for (size_t i = 0; i < bs * 2; ++i) {
                    *dst++ = '\\';
                }
                bs = 0;
                *dst++ = '\\';
                *dst++ = '"';
            } else {
                for (size_t i = 0; i < bs; ++i) {
                    *dst++ = '\\';
                }
                bs = 0;
                *dst++ = *p;
            }
        }
        for (size_t i = 0; i < bs * 2; ++i) {
            *dst++ = '\\';
        }
        *dst++ = '"';
    }

    *dst = '\0';
    *len = (size_t)(dst - *buf);
    return 0;
}

static char *build_win_cmdline(const char *const argv[]) {
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (size_t i = 0; argv[i] != NULL; ++i) {
        if (append_win_arg(&buf, &len, &cap, argv[i]) != 0) {
            free(buf);
            return NULL;
        }
    }

    if (!buf) {
        buf = (char *)malloc(1);
        if (buf) {
            buf[0] = '\0';
        }
    }
    return buf;
}

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || argv[0][0] == '\0' || !proc) {
        return -1;
    }

    char *cmd = build_win_cmdline(argv);
    if (!cmd) {
        return -1;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessA(
        NULL,
        cmd,
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );

    free(cmd);

    if (!ok) {
        return -1;
    }

    CloseHandle(pi.hThread);
    proc->handle = (intptr_t)pi.hProcess;
    proc->pid = (long)pi.dwProcessId;
    return 0;
}

static int win_collect_exit_code(HANDLE h, int *exit_code) {
    DWORD raw_code = 0;
    if (!GetExitCodeProcess(h, &raw_code)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    if (exit_code) {
        *exit_code = (int)raw_code;
    }
    return 0;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || proc->handle == 0 || proc->handle == (intptr_t)INVALID_HANDLE_VALUE) {
        return -1;
    }

    HANDLE h = (HANDLE)proc->handle;
    DWORD wait_res = WaitForSingleObject(h, INFINITE);
    if (wait_res != WAIT_OBJECT_0) {
        CloseHandle(h);
        proc->handle = 0;
        return -1;
    }

    proc->handle = 0;
    return win_collect_exit_code(h, exit_code);
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || proc->handle == 0 || proc->handle == (intptr_t)INVALID_HANDLE_VALUE) {
        return -1;
    }

    HANDLE h = (HANDLE)proc->handle;
    DWORD wait_res = WaitForSingleObject(h, 0);
    if (wait_res == WAIT_TIMEOUT) {
        return 0;
    }
    if (wait_res != WAIT_OBJECT_0) {
        CloseHandle(h);
        proc->handle = 0;
        return -1;
    }

    proc->handle = 0;
    if (win_collect_exit_code(h, exit_code) != 0) {
        return -1;
    }
    return 1;
}

#else /* POSIX */

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static ssize_t write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return (ssize_t)count;
}

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || argv[0][0] == '\0' || !proc) {
        return -1;
    }

    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        return -1;
    }

    if (set_cloexec(err_pipe[0]) != 0 || set_cloexec(err_pipe[1]) != 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(err_pipe[0]);
        execvp(argv[0], (char *const *)argv);
        int child_err = errno;
        (void)write_all(err_pipe[1], &child_err, sizeof(child_err));
        close(err_pipe[1]);
        _exit(127);
    }

    close(err_pipe[1]);

    int child_err = 0;
    ssize_t r;
    do {
        r = read(err_pipe[0], &child_err, sizeof(child_err));
    } while (r < 0 && errno == EINTR);
    close(err_pipe[0]);

    if (r == (ssize_t)sizeof(child_err)) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0) {
            if (errno != EINTR) {
                break;
            }
        }
        errno = child_err;
        return -1;
    }

    proc->handle = -1;
    proc->pid = (long)pid;
    return 0;
}

static int decode_posix_status(int status, int *exit_code) {
    int code = -1;
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        code = 128 + WTERMSIG(status);
    } else {
        return -1;
    }
    if (exit_code) {
        *exit_code = code;
    }
    return 0;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || proc->pid <= 0) {
        return -1;
    }

    pid_t target = (pid_t)proc->pid;
    int status = 0;
    pid_t w;
    do {
        w = waitpid(target, &status, 0);
    } while (w < 0 && errno == EINTR);

    proc->pid = 0;
    if (w < 0) {
        return -1;
    }

    return decode_posix_status(status, exit_code);
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || proc->pid <= 0) {
        return -1;
    }

    pid_t target = (pid_t)proc->pid;
    int status = 0;
    pid_t w;
    do {
        w = waitpid(target, &status, WNOHANG);
    } while (w < 0 && errno == EINTR);

    if (w == 0) {
        return 0;
    }
    if (w < 0) {
        return -1;
    }

    proc->pid = 0;
    if (decode_posix_status(status, exit_code) != 0) {
        return -1;
    }
    return 1;
}

#endif