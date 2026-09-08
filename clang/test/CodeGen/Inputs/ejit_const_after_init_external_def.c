__attribute__((ejit_const_after_init)) unsigned g_external;

void set_external(unsigned value) { g_external = value; }
