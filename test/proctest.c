#include "proclaunch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static void sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0) {
    }
#endif
}

static int run_child_mode(int argc, char *argv[]) {
    int code = atoi(argv[2]);
    int ms = atoi(argv[3]);
    sleep_ms(ms);

    if (argc >= 5) {
        printf("%s\n", argv[4]);
        fflush(stdout);
    }
    return code;
}

static int test_poll_while_running(const char *self) {
    pl_process proc;
    const char *const argv[] = {self, "--child", "42", "1000", NULL};

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 1 (pl_poll): pl_start вернула -1\n");
        return 0;
    }

    int iterations = 0;
    int status = 0;
    int code = -1;

    while ((status = pl_poll(&proc, &code)) == 0) {
        iterations++;
        sleep_ms(100);
        if (iterations > 100) {
            printf("[FAIL] Тест 1 (pl_poll): превышено число итераций\n");
            return 0;
        }
    }

    if (status != 1 || iterations <= 3 || code != 42) {
        printf("[FAIL] Тест 1 (pl_poll): iterations=%d, status=%d, code=%d (ожидалось >3 и code=42)\n",
               iterations, status, code);
        return 0;
    }

    printf("[ OK ] Тест 1 (pl_poll): итераций=%d, код завершения=%d\n", iterations, code);
    return 1;
}

static int test_wait_exit_code(const char *self) {
    pl_process proc;
    const char *const argv[] = {self, "--child", "7", "300", NULL};

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 2 (pl_wait): pl_start вернула -1\n");
        return 0;
    }

    int code = -1;
    if (pl_wait(&proc, &code) != 0 || code != 7) {
        printf("[FAIL] Тест 2 (pl_wait): получен код %d (ожидался 7)\n", code);
        return 0;
    }

    printf("[ OK ] Тест 2 (pl_wait): код завершения=%d\n", code);
    return 1;
}

static int test_space_and_quotes_arg(const char *self) {
    const char *special = "hello \"big\" world\\";
    pl_process proc;
    const char *const argv[] = {self, "--child", "0", "50", special, NULL};

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 3 (аргумент с пробелами и кавычками): pl_start вернула -1\n");
        return 0;
    }

    int code = -1;
    if (pl_wait(&proc, &code) != 0 || code != 0) {
        printf("[FAIL] Тест 3: ошибка ожидания ребёнка, код=%d\n", code);
        return 0;
    }

    printf("[ OK ] Тест 3 (аргумент): передан \"%s\"\n", special);
    return 1;
}

static int test_missing_program(void) {
    pl_process proc;
    const char *const argv[] = {"no_such_program_12345", NULL};

    int rc = pl_start(argv, &proc);
    if (rc != -1) {
        printf("[FAIL] Тест 4 (несуществующая программа): ожидался -1, получено %d\n", rc);
        return 0;
    }

    printf("[ OK ] Тест 4 (несуществующая программа): pl_start корректно вернула -1\n");
    return 1;
}

static int test_exit_code_300(const char *self) {
    pl_process proc;
    const char *const argv[] = {self, "--child", "300", "50", NULL};

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 5 (код 300): pl_start вернула -1\n");
        return 0;
    }

    int code = -1;
    if (pl_wait(&proc, &code) != 0) {
        printf("[FAIL] Тест 5 (код 300): pl_wait вернула -1\n");
        return 0;
    }

#ifdef _WIN32
    int expected = 300;
#else
    int expected = 44;
#endif

    if (code != expected) {
        printf("[FAIL] Тест 5 (код 300): получен код %d (ожидался %d)\n", code, expected);
        return 0;
    }

    printf("[ OK ] Тест 5 (код 300): получен код %d\n", code);
    return 1;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc >= 4 && strcmp(argv[1], "--child") == 0) {
        return run_child_mode(argc, argv);
    }

    const char *self = (argc > 0 && argv[0]) ? argv[0] : "proctest";
    int passed = 0;
    const int total = 5;

    passed += test_poll_while_running(self);
    passed += test_wait_exit_code(self);
    passed += test_space_and_quotes_arg(self);
    passed += test_missing_program();
    passed += test_exit_code_300(self);

    printf("Итог: %d/%d тестов пройдено.\n", passed, total);
    return (passed == total) ? 0 : 1;
}