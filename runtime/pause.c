// The pause: the game stands still while the in-game menu is open (AC_MENU_PAUSE, on by default).
//
// It is taken at a present (nvn_api_obj.c nvnQueuePresentTexture), so the picture that stays is a whole frame, and it
// stops the whole guest rather than its frame loop:
//   - kern_pause (kernel.c) has every thread give its virtual core up at a point where it could be preempted, grants
//     none until kern_resume, and only then stops the guest's clock. No guest code runs, so nothing spins on a clock
//     that stands still, and the game, its timed waits, Bink and the sound engine see no time pass.
//   - The audio output (ov_audout.c out_fill) keeps what the game queued and plays silence, so sound and picture go on
//     from the same point.
//   - The presenting thread, which holds no core here, shows the same picture again (GfxBackend repeat): the menu
//     and the performance overlay are drawn by the present path and need presents. It also polls the input, for the
//     pad's chord that closes the menu: the game, which polls it otherwise, is stopped.
//     The repeats stay a tenth under the display's rate, 54 a second at 60 Hz (repeat_period): at the display's own
//     rate a present that was once early stays queued, and the driver then waits in every vkQueuePresentKHR. Intel's
//     spins there (igvk64, mailbox and fifo): on an Arc 140T the paused game took a whole core that way, from the
//     second pause of a run on. They go out as soon as they are drawn, without the present thread's last
//     millisecond of yielding to a display time, which a still picture does not need.
//
// Host threads go on: the window, the present and replay threads, pipeline builds, a save's Commit. A host call under
// way finishes, and its thread stops where it takes its core back. So no host lock that those threads need may be
// held across kern_core_back or a call into guest code.
//
// The pause waits for a save being written (fs_save_busy), so that quitting from the paused menu never cuts one in
// two, and it is taken at the next present after it.
//
// A setting whose effect is drawn by the game (the upscaler, ambient occlusion, the HUD) shows once the game goes on;
// what the present path does (colour, sharpening, HDR levels, the window) shows at once.
//
// Another reason to pause goes into pause_wanted.
#include "cfg.h"
#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "pause.h"
#include "platform/platform.h"
#include "ui/menu.h"

#define PAUSE_RETRY_NS 2000000000ull          // after a guest that did not stop (kern_pause): the game runs on this long

static int pause_wanted(void) { return menu_is_open() && cfg_bool("AC_MENU_PAUSE"); }

// The time between two repeats: 1.1 periods of the display, or of 60 Hz when that is faster or unknown.
static uint64_t repeat_period(void) {
    double hz = gfx_window_refresh_hz();
    if (hz < 20 || hz > 60) hz = 60;
    return (uint64_t)(1.1e9 / hz);
}

void pause_frame(const tex_t *t) {
    static uint64_t retry_ns;   // presenting thread only
    if (__builtin_expect(!pause_wanted(), 1)) return;
    const GfxBackend *b = gfx_backend();
    if (!b->repeat || fs_save_busy()) return;
    uint64_t asked = kern_now_ns();
    if (asked < retry_ns) return;
    if (!kern_pause()) {
        retry_ns = kern_now_ns() + PAUSE_RETRY_NS;
        return;
    }
    if (fs_save_busy()) {   // a save began while the cores were coming back
        kern_resume();
        return;
    }
    uint64_t start = kern_now_ns(), next = start;
    rt_log("pause: the game stands still (its threads stopped in %.2f ms)", (double)(start - asked) / 1e6);
    while (pause_wanted()) {
        next += repeat_period();
        uint64_t now = kern_now_ns();
        if (next < now) next = now;
        b->repeat(t, 0);
        input_poll_paused();
        now = kern_now_ns();
        if (next > now) plat_wait_ns(next - now);
    }
    lowlat_resume();
    kern_resume();
    rt_log("pause: the game goes on after %.2f s", (double)(kern_now_ns() - start) / 1e9);
}
