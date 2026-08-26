#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(const char *what, int ok) {
    printf("[dlopen_test] %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

int main(void) {
    printf("[dlopen_test] === runtime DSO loading ===\n");

    void *missing = dlopen("libdoes-not-exist.so", RTLD_NOW | RTLD_LOCAL);
    check("a missing DSO fails without terminating the process", !missing);
    check("dlerror() explains the missing DSO", dlerror() != NULL);

    void *dependency_handle = dlopen("libextron_rtld_dep.so", RTLD_NOW | RTLD_LOCAL);
    int (*dependency_value)(void) = dependency_handle
        ? (int (*)(void))dlsym(dependency_handle, "extron_dep_value") : NULL;
    void (*set_dependency_destructor_counter)(int *) = dependency_handle
        ? (void (*)(int *))dlsym(dependency_handle,
                                 "extron_dep_set_destructor_counter") : NULL;
    check("the dependency DSO loads directly", dependency_value
          && dependency_value() == 8);

    void *handle = dlopen("libextron_rtld_test.so", RTLD_NOW | RTLD_LOCAL);
    void *second_handle = dlopen("libextron_rtld_test.so", RTLD_NOW | RTLD_LOCAL);
    check("dlopen() loads a DSO absent from DT_NEEDED", handle != NULL);
    check("a second dlopen() returns the existing DSO", second_handle == handle);
    if (!handle) {
        const char *error = dlerror();
        printf("[dlopen_test] loader error: %s\n", error ? error : "(none)");
        return 1;
    }

    int (*value)(void) = (int (*)(void))dlsym(handle, "extron_dso_value");
    int *global = (int *)dlsym(handle, "extron_dso_global");
    int *tls = (int *)dlsym(handle, "extron_dso_tls");
    int **relro_pointer = (int **)dlsym(handle, "extron_dso_relro_pointer");
    int (*dependency_through_root)(void) =
        (int (*)(void))dlsym(handle, "extron_dso_dependency_value");
    void (*set_destructor_counter)(int *) =
        (void (*)(int *))dlsym(handle, "extron_dso_set_destructor_counter");
    check("dlsym() resolves an exported function", value != NULL);
    check("dlsym() resolves exported data", global && *global == 22);
    check("the late-loaded DSO constructor ran", value && value() == 33);
    check("dlsym() resolves the current thread's TLS instance", tls && *tls == 11);
    check("dlsym() resolves data stored in the DSO's RELRO segment",
          relro_pointer && *relro_pointer == global);
    check("the root DSO's DT_NEEDED dependency is callable",
          dependency_through_root && dependency_through_root() == 8);

    pid_t child = fork();
    if (child == 0) {
        if (!relro_pointer)
            _exit(2);
        *relro_pointer = global;
        _exit(0);
    }
    int status = 0;
    int relro_protected = child > 0 && waitpid(child, &status, 0) == child
                       && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    check("late-loaded DSO RELRO faults only the writing child", relro_protected);

    void *unknown = dlsym(handle, "symbol_that_does_not_exist");
    check("an unknown symbol fails", unknown == NULL);
    check("dlerror() explains the unknown symbol", dlerror() != NULL);

    int destructor_count = 0;
    int dependency_destructor_count = 0;
    if (set_destructor_counter)
        set_destructor_counter(&destructor_count);
    if (set_dependency_destructor_counter)
        set_dependency_destructor_counter(&dependency_destructor_count);
    check("closing a direct dependency reference keeps it reachable",
          dependency_handle && dlclose(dependency_handle) == 0
          && dependency_destructor_count == 0
          && dependency_through_root && dependency_through_root() == 8);
    check("first dlclose() only drops one reference", dlclose(handle) == 0);
    check("the first close keeps code mapped and skips destructors",
          destructor_count == 0 && value && value() == 33);
    check("final dlclose() runs the DSO destructor", dlclose(second_handle) == 0
          && destructor_count == 1);
    check("final root close also destroys its unreachable dependency",
          dependency_destructor_count == 1);

    void *stale = dlsym(second_handle, "extron_dso_value");
    check("dlsym() rejects a closed handle", stale == NULL && dlerror() != NULL);

    child = fork();
    if (child == 0) {
        volatile unsigned char byte = *(volatile unsigned char *)(void *)value;
        (void)byte;
        _exit(0);
    }
    status = 0;
    int unmapped = child > 0 && waitpid(child, &status, 0) == child
                && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    check("final dlclose() really unmaps the DSO", unmapped);

    child = fork();
    if (child == 0) {
        volatile unsigned char byte =
            *(volatile unsigned char *)(void *)dependency_value;
        (void)byte;
        _exit(0);
    }
    status = 0;
    int dependency_unmapped = child > 0 && waitpid(child, &status, 0) == child
                           && WIFSIGNALED(status)
                           && WTERMSIG(status) == SIGSEGV;
    check("unreachable DT_NEEDED dependency is really unmapped",
          dependency_unmapped);

    check("a repeated close is rejected",
          dlclose(second_handle) != 0 && dlerror() != NULL);

    void *reopened = dlopen("libextron_rtld_test.so", RTLD_NOW | RTLD_LOCAL);
    int (*reopened_value)(void) = reopened
        ? (int (*)(void))dlsym(reopened, "extron_dso_value") : NULL;
    check("the unloaded DSO can be loaded and initialized again",
          reopened && reopened_value && reopened_value() == 33);
    check("the reopened DSO can be unloaded again", reopened && dlclose(reopened) == 0);

    printf("[dlopen_test] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
