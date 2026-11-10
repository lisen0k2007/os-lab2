#define _POSIX_C_SOURCE 200809L

#include "proclaunch.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* ======================= WINDOWS ======================= */
#include <windows.h>

/* Экранирование одного аргумента по правилам командной строки Windows.
 * Возвращает 0 при успехе. Пишет в *out (malloc), *len - длина. */
static int append_quoted(char **buf, size_t *cap, size_t *len, const char *arg)
{
    int need_quotes = (*arg == '\0');
    for (const char *p = arg; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '"') { need_quotes = 1; break; }
    }

    size_t need = strlen(arg) * 2 + 4; /* запас под экранирование и кавычки */
    if (*len + need + 1 > *cap) {
        size_t ncap = (*len + need + 1) * 2;
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    char *b = *buf + *len;

    if (need_quotes) *b++ = '"';

    size_t backslashes = 0;
    for (const char *p = arg; ; ++p) {
        if (*p == '\\') {
            ++backslashes;
        } else if (*p == '"') {
            /* удвоить обратные слеши, экранировать кавычку */
            for (size_t i = 0; i < backslashes * 2 + 1; ++i) *b++ = '\\';
            *b++ = '"';
            backslashes = 0;
        } else if (*p == '\0') {
            /* перед закрывающей кавычкой удвоить оставшиеся слеши */
            if (need_quotes) {
                for (size_t i = 0; i < backslashes * 2; ++i) *b++ = '\\';
                *b++ = '"';
            } else {
                for (size_t i = 0; i < backslashes; ++i) *b++ = '\\';
            }
            break;
        } else {
            for (size_t i = 0; i < backslashes; ++i) *b++ = '\\';
            backslashes = 0;
            *b++ = *p;
        }
    }

    *len = (size_t)(b - *buf);
    (*buf)[*len] = '\0';
    return 0;
}

static char *build_cmdline(const char *const argv[])
{
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    for (size_t i = 0; argv[i] != NULL; ++i) {
        if (i > 0) {
            if (len + 2 > cap) {
                char *nb = (char *)realloc(buf, cap * 2);
                if (!nb) { free(buf); return NULL; }
                buf = nb; cap *= 2;
            }
            buf[len++] = ' ';
            buf[len] = '\0';
        }
        if (append_quoted(&buf, &cap, &len, argv[i]) != 0) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

int pl_start(const char *const argv[], pl_process *proc)
{
    if (!argv || !argv[0] || !proc) return -1;

    char *cmd = build_cmdline(argv);
    if (!cmd) return -1;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD err = GetLastError();
    free(cmd);

    if (!ok) {
        (void)err; /* err == 2: файл не найден */
        return -1;
    }

    CloseHandle(pi.hThread);
    proc->handle = (intptr_t)pi.hProcess;
    proc->pid = (long)pi.dwProcessId;
    return 0;
}

int pl_wait(pl_process *proc, int *exit_code)
{
    if (!proc || !exit_code) return -1;
    HANDLE h = (HANDLE)proc->handle;
    if (h == NULL) return -1;

    DWORD wr = WaitForSingleObject(h, INFINITE);
    if (wr != WAIT_OBJECT_0) return -1;

    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) { CloseHandle(h); return -1; }
    CloseHandle(h);
    proc->handle = 0;

    *exit_code = (int)code;
    return 0;
}

int pl_poll(pl_process *proc, int *exit_code)
{
    if (!proc || !exit_code) return -1;
    HANDLE h = (HANDLE)proc->handle;
    if (h == NULL) return -1;

    DWORD wr = WaitForSingleObject(h, 0);
    if (wr == WAIT_TIMEOUT) return 0;
    if (wr != WAIT_OBJECT_0) return -1;

    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) { CloseHandle(h); return -1; }
    CloseHandle(h);
    proc->handle = 0;

    *exit_code = (int)code;
    return 1;
}

#else
/* ======================= POSIX ======================= */
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int pl_start(const char *const argv[], pl_process *proc)
{
    if (!argv || !argv[0] || !proc) return -1;

    int fds[2];
    if (pipe(fds) != 0) return -1;

    /* write-end должен закрыться при успешном exec */
    int flags = fcntl(fds[1], F_GETFD);
    if (flags < 0 || fcntl(fds[1], F_SETFD, flags | FD_CLOEXEC) < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        /* ребёнок */
        close(fds[0]);
        execvp(argv[0], (char *const *)argv);
        /* exec не удался: сообщим errno родителю */
        int err = errno;
        ssize_t w = write(fds[1], &err, sizeof err);
        (void)w;
        _exit(127);
    }

    /* родитель */
    close(fds[1]);

    int child_errno = 0;
    ssize_t n;
    do {
        n = read(fds[0], &child_errno, sizeof child_errno);
    } while (n < 0 && errno == EINTR);
    close(fds[0]);

    if (n == (ssize_t)sizeof child_errno) {
        /* exec провалился: дождёмся ребёнка и вернём ошибку */
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
        return -1;
    }

    proc->pid = (long)pid;
    proc->handle = -1;
    return 0;
}

/* Разбор статуса */
static int decode_status(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);          /* 0..255 */
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int pl_wait(pl_process *proc, int *exit_code)
{
    if (!proc || !exit_code || proc->pid <= 0) return -1;

    int st;
    pid_t r;
    do {
        r = waitpid((pid_t)proc->pid, &st, 0);
    } while (r < 0 && errno == EINTR);

    if (r < 0) return -1;
    proc->pid = 0;
    *exit_code = decode_status(st);
    return 0;
}

int pl_poll(pl_process *proc, int *exit_code)
{
    if (!proc || !exit_code || proc->pid <= 0) return -1;

    int st;
    pid_t r;
    do {
        r = waitpid((pid_t)proc->pid, &st, WNOHANG);
    } while (r < 0 && errno == EINTR);

    if (r < 0) return -1;
    if (r == 0) return 0; /* ещё работает */

    proc->pid = 0;
    *exit_code = decode_status(st);
    return 1;
}

#endif /* _WIN32 */