int extron_dep_global = 7;
static int *destructor_counter;

__attribute__((constructor))
static void extron_dep_constructor(void) {
    ++extron_dep_global;
}

__attribute__((destructor))
static void extron_dep_destructor(void) {
    if (destructor_counter)
        ++*destructor_counter;
}

void extron_dep_set_destructor_counter(int *counter) {
    destructor_counter = counter;
}

int extron_dep_value(void) {
    return extron_dep_global;
}
