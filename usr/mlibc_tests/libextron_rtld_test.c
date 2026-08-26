int extron_dso_global = 20;
_Thread_local int extron_dso_tls = 10;

__attribute__((constructor))
static void extron_dso_constructor(void) {
    extron_dso_global += 2;
    extron_dso_tls += 1;
}

int extron_dso_value(void) {
    return extron_dso_global + extron_dso_tls;
}
