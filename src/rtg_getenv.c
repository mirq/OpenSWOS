/*
 * rtg_getenv.c -- getenv/setenv/unsetenv for the 68060 RTG build
 * (docs/RTG060.md). Empty everywhere else.
 *
 * libnix's getenv searches its own environ table and never asks dos.library, so
 * an ENV: variable -- SWOS_SCREENMODE, SWOS_AUTO_MATCH, every SWOS_* switch the
 * WarpOS build reads through newlib -- was invisible. This one is GetVar: a
 * Shell-local variable first, then ENV:. setenv is SetVar of a LOCAL variable
 * (this process's own, gone when it ends), so what the game sets for itself
 * (main.c's switches) is what getenv finds, and ENV: is never written. Defined
 * in the program, they are linked instead of the library's.
 *
 * The C rule is that the string stays valid until the next call; callers here
 * keep it longer (a pointer saved at start-up), so each name gets storage of its
 * own that lives as long as the program, rewritten only when the value changes.
 */
#if defined(SWOS_RTG060_BUILD)

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#pragma pack(push, 2)
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/dos.h>
#pragma pack(pop)

struct EnvSlot {
    struct EnvSlot *next;
    char           *value;
    int             cap;
    char            name[1];
};
static struct EnvSlot *s_slots;

char *getenv(const char *name)
{
    char buf[1024];
    struct EnvSlot *s;
    LONG len;
    if (!name || !*name)
        return NULL;
    len = GetVar((CONST_STRPTR)name, (STRPTR)buf, sizeof buf, 0);
    if (len < 0)
        return NULL;
    for (s = s_slots; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            break;
    if (!s) {
        if (!(s = (struct EnvSlot *)calloc(1, sizeof *s + strlen(name))))
            return NULL;
        strcpy(s->name, name);
        s->next = s_slots;
        s_slots = s;
    }
    if (!s->value || strcmp(s->value, buf) != 0) {
        if (len + 1 > s->cap) {                 /* a longer value: the old one stays allocated */
            char *v = (char *)malloc((size_t)len + 1);
            if (!v)
                return s->value;
            s->value = v;
            s->cap = (int)len + 1;
        }
        memcpy(s->value, buf, (size_t)len + 1);
    }
    return s->value;
}

int setenv(const char *name, const char *value, int overwrite)
{
    char probe[2];
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && GetVar((CONST_STRPTR)name, (STRPTR)probe, sizeof probe, 0) >= 0)
        return 0;
    if (!SetVar((CONST_STRPTR)name, (CONST_STRPTR)(value ? value : ""), -1,
                GVF_LOCAL_ONLY | LV_VAR)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int unsetenv(const char *name)
{
    if (name && *name)
        DeleteVar((CONST_STRPTR)name, GVF_LOCAL_ONLY | LV_VAR);
    return 0;
}

#endif /* SWOS_RTG060_BUILD */
