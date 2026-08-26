int extron_dso_global = 20;
_Thread_local int extron_dso_tls = 10;
int * const extron_dso_relro_pointer = &extron_dso_global;
static int *destructor_counter;
extern int extron_dep_value(void);

__attribute__((constructor))
static void extron_dso_constructor(void) {
    extron_dso_global += 2;
    extron_dso_tls += 1;
}

__attribute__((destructor))
static void extron_dso_destructor(void) {
    if (destructor_counter)
        ++*destructor_counter;
}

void extron_dso_set_destructor_counter(int *counter) {
    destructor_counter = counter;
}

int extron_dso_value(void) {
    return extron_dso_global + extron_dso_tls;
}

int extron_dso_dependency_value(void) {
    return extron_dep_value();
}
