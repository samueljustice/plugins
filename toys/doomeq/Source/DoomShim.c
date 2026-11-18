/*
 * Keeps DOOM from taking the DAW down with it.
 *
 * The engine sources are compiled with -Dexit=dg_exit, so every exit() inside DOOM
 * (I_Error, bad WAD, "-help" and friends) lands here and jumps back to the host
 * instead of terminating the process.
 */
#include <setjmp.h>
#include "doomgeneric.h"
#include "doomstat.h"
#include "s_sound.h"

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

/* The host asks for the game to be paused (leaving command mode) or not. Applied here, on
   the DOOM thread, so there's no racing against queued key presses. Only a pause the host
   made is undone by the host; one the player made with the Pause key stays. */
static volatile int dg_host_wants_pause = 0;
static int dg_host_paused = 0;

void dg_request_pause (int wanted)
{
    dg_host_wants_pause = wanted;
}

static void dg_apply_pause (void)
{
    const int inGame = usergame && gamestate == GS_LEVEL && ! demoplayback;

    if (! paused)
        dg_host_paused = 0;

    if (dg_host_wants_pause && inGame && ! paused && ! menuactive)
    {
        paused = true;
        S_PauseSound();
        dg_host_paused = 1;
    }
    else if (! dg_host_wants_pause && paused && dg_host_paused)
    {
        paused = false;
        S_ResumeSound();
        dg_host_paused = 0;
    }
}

int dg_safe_tick (void)
{
    dg_apply_pause();

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

/* 1: the player is in a level of their own game, 2: paused, 4: the menu is open,
   8: the pause was made by the host. */
int dg_game_flags (void)
{
    int flags = 0;
    if (usergame && gamestate == GS_LEVEL && ! demoplayback) flags |= 1;
    if (paused)     flags |= 2;
    if (menuactive) flags |= 4;
    if (paused && dg_host_paused) flags |= 8;
    return flags;
}
