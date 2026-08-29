/* Parser smoke test: reads a .class and prints what ps_jclass made of it. */
#include "ps_jclass.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f; long sz; uint8_t *buf; const char *err = NULL; ps_jclass *c;
    int i;

    if(argc < 2) { fprintf(stderr, "usage: jclass_dump X.class\n"); return 2; }
    f = fopen(argv[1], "rb");
    if(!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)sz + 1);
    if(fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr,"short read\n"); return 1; }
    fclose(f);

    c = ps_jclass_parse(buf, (size_t)sz, &err);
    if(!c) { fprintf(stderr, "parse failed: %s\n", err ? err : "?"); return 1; }

    printf("class      %s\n", c->name);
    printf("super      %s\n", c->super_name ? c->super_name : "(none)");
    printf("cp entries %u\n", c->cp_count);
    printf("fields     %u\n", c->field_count);
    for(i = 0; i < c->field_count; i++)
        printf("   %-12s %-24s kind=%d\n", c->fields[i].name, c->fields[i].desc,
               c->fields[i].kind);
    printf("methods    %u\n", c->method_count);
    for(i = 0; i < c->method_count; i++) {
        ps_jmethod *m = &c->methods[i];
        printf("   %-12s %-28s args=%u ret=%d stack=%u locals=%u code=%u\n",
               m->name, m->desc, m->arg_slots, m->ret_kind,
               m->max_stack, m->max_locals, m->code_len);
    }
    ps_jclass_free(c);
    return 0;
}
