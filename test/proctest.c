#define _POSIX_C_SOURCE 200809L

#include "proclaunch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static int g_fail = 0;

static void report(const char *name, int ok, const char *detail)
{
    printf("[%s] %s%s%s\n", ok ? " OK " : "FAIL", name,
           (detail && *detail) ? ": " : "", detail ? detail : "");
    if (!ok) g_fail = 1;
}

/* ---- Тест 1: фон + poll ---- */
static void test_background_poll(const char *self)
{
    char codebuf[32], msbuf[32];
    snprintf(codebuf, sizeof codebuf, "42");
    snprintf(msbuf,  sizeof msbuf,  "1000");

    const char *argv[] = { self, "--child", codebuf, msbuf, NULL };
    pl_process p;
    if (pl_start(argv, &p) != 0) { report("test1 start", 0, "pl_start failed"); return; }

    int iterations = 0, code = -1, done = 0;
    while (iterations < 50) {
        int r = pl_poll(&p, &code);
        if (r < 0) break;
        if (r == 1) { done = 1; break; }
        iterations++;
        sleep_ms(100);
    }

    char detail[128];
    snprintf(detail, sizeof detail, "iterations=%d, code=%d", iterations, code);
    report("test1 background poll", done && iterations > 3 && code == 42, detail);

    if (!done) { pl_wait(&p, &code); }
}

/* ---- Тест 2: wait ---- */
static void test_wait(const char *self)
{
    const char *argv[] = { self, "--child", "7", "300", NULL };
    pl_process p;
    if (pl_start(argv, &p) != 0) { report("test2 start", 0, "pl_start failed"); return; }

    int code = -1;
    int r = pl_wait(&p, &code);
    char detail[64];
    snprintf(detail, sizeof detail, "code=%d", code);
    report("test2 wait", r == 0 && code == 7, detail);
}

/* ---- Тест 3: аргумент с пробелами и кавычками ---- */
static void test_spaced_arg(const char *self)
{
    /* Ребёнок получит 4-й аргумент и сравнит с ожидаемым */
    const char *expected = "hello \"big\" world\\";
    const char *argv[] = { self, "--child-echo", expected, NULL };
    pl_process p;
    if (pl_start(argv, &p) != 0) { report("test3 start", 0, "pl_start failed"); return; }

    int code = -1;
    int r = pl_wait(&p, &code);
    /* ребёнок вернёт 0, если аргумент совпал */
    char detail[128];
    snprintf(detail, sizeof detail, "code=%d (0=OK)", code);
    report("test3 spaced argument", r == 0 && code == 0, detail);
}

/* ---- Тест 4: несуществующая программа ---- */
static void test_missing_program(void)
{
    const char *argv[] = { "no_such_program_12345", NULL };
    pl_process p;
    int r = pl_start(argv, &p);
    report("test4 missing program", r == -1, r == -1 ? "pl_start returned -1" : "unexpected success");
}

/* ---- Тест 5: код 300 (Linux: 44, Windows: 300) ---- */
static void test_exit_code_300(const char *self)
{
    const char *argv[] = { self, "--child", "300", "10", NULL };
    pl_process p;
    if (pl_start(argv, &p) != 0) { report("test5 start", 0, "pl_start failed"); return; }

    int code = -1;
    int r = pl_wait(&p, &code);
#ifdef _WIN32
    int expected = 300;
#else
    int expected = 300 & 0xFF; /* 44 */
#endif
    char detail[64];
    snprintf(detail, sizeof detail, "code=%d expected=%d", code, expected);
    report("test5 exit code 300", r == 0 && code == expected, detail);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    /* Режим ребёнка: --child <code> <ms> */
    if (argc >= 4 && strcmp(argv[1], "--child") == 0) {
        int code = atoi(argv[2]);
        int ms   = atoi(argv[3]);
        sleep_ms(ms);
        return code;
    }

    /* Режим ребёнка-эха: --child-echo <arg> */
    if (argc >= 3 && strcmp(argv[1], "--child-echo") == 0) {
        const char *expected = "hello \"big\" world\\";
        if (strcmp(argv[2], expected) == 0) {
            printf("child got: [%s]\n", argv[2]);
            return 0;
        }
        printf("child got WRONG: [%s]\n", argv[2]);
        return 1;
    }

    const char *self = argv[0];
    printf("self = %s\n\n", self);

    test_background_poll(self);
    test_wait(self);
    test_spaced_arg(self);
    test_missing_program();
    test_exit_code_300(self);

    printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}