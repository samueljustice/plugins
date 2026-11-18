/*
 * Keeps DOOM from taking the DAW down with it.
 *
 * The engine sources are compiled with -Dexit=dg_exit, so every exit() inside DOOM
 * (I_Error, bad WAD, "-help" and friends) lands here and jumps back to the host
 * instead of terminating the process.
 */
#include <setjmp.h>
#include "doomgeneric.h"

const char* dg_config_dir = 0;

static jmp_buf dg_jump;
static volatile int dg_armed = 0;

#if defined(_MSC_VER)
__declspec(noreturn) void dg_exit (int code)
#else
__attribute__((noreturn)) void dg_exit (int code)
#endif
{
    (void) code;
    if (dg_armed)
        longjmp (dg_jump, 1);
    for (;;) {}
}

int dg_safe_create (int argc, char** argv)
{
    dg_armed = 1;
    if (setjmp (dg_jump) != 0)
    {
        dg_armed = 0;
        return 1;
    }
    doomgeneric_Create (argc, argv);
    dg_armed = 0;
    return 0;
}

int dg_safe_tick (void)
{
    dg_armed = 1;
    if (setjmp (dg_jump) != 0)
    {
        dg_armed = 0;
        return 1;
    }
    doomgeneric_Tick();
    dg_armed = 0;
    return 0;
}
