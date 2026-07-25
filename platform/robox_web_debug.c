// sdk/robox_web_debug.c -- crash and hang reporting for the Emscripten build.
//
// WHY THIS FILE EXISTS
//
// Everywhere else, a host-side fault inside recompiled guest code arrives as a
// signal and src/main.c's on_fatal_signal prints a report. WebAssembly has no
// signals. A wasm trap -- out-of-bounds access, call_indirect signature
// mismatch, `unreachable`, or the engine's own stack-exhaustion RangeError --
// throws a JavaScript exception that unwinds straight through every C frame to
// the nearest JS caller. signal(SIGSEGV, ...) is never consulted; it is dead
// code on this target.
//
// With PROXY_TO_PTHREAD the nearest JS caller is emscripten's pthread worker
// entry, which rethrows to the worker's global error handler. The main thread
// logs "worker sent an error!" and the page freezes on its last frame. That is
// the entire diagnostic we had: a still image and one useless sentence.
//
// So the guest entry point is called through a JS trampoline that owns a
// try/catch. When the trap unwinds into it we still have everything we need:
// the wasm instance is intact (a trap poisons neither memory nor the table),
// g_cpu still holds the guest register file, and the exception carries a host
// stack trace. Because every recompiled function is literally named
// func_<guest-VA>, that host stack trace IS a guest backtrace -- provided the
// wasm name section survives the link, which is what --profiling-funcs in
// CMakeLists.txt is for.
//
// The hang watchdog is separate and pure JS: it runs on the browser main
// thread (idle under PROXY_TO_PTHREAD) and reads the guest's flip counter and
// LR straight out of the shared heap. It deliberately never calls into wasm --
// the guest worker may be holding the stdio lock, and a watchdog that
// deadlocks the page it is diagnosing is worse than no watchdog.

#if defined(__EMSCRIPTEN__)

#include "hle.h"

#include <emscripten/emscripten.h>
#include <emscripten/em_asm.h>
#include <emscripten/threading.h>

#include <stdio.h>
#include <stdint.h>

extern void robox_prof_crash_dump(const char *reason);

// The entry the trampoline calls. Stashed rather than passed through JS so the
// function pointer never has to be materialised into the wasm table.
static PPC_Func g_web_entry;

// Called from the JS catch below. Prints the guest-side half of the report:
// crashing guest address, registers, backtrace, recent dispatches. The
// JS-side half (the host stack trace) is printed by the trampoline.
EMSCRIPTEN_KEEPALIVE void robox_web_on_crash(void)
{
    robox_prof_crash_dump("host exception (wasm trap) -- see host stack below");
}

EMSCRIPTEN_KEEPALIVE void robox_web_call_entry(void)
{
    if (g_web_entry) g_web_entry();
}

// Park the guest thread instead of letting main() return.
//
// Returning unwinds the runtime, and every later mouse/keyboard event then
// tries to proxy its SDL callback to a thread that no longer exists --
// "emscripten_proxy_async failed" aborts, once per event, burying the crash
// report under a wall of unrelated assertions. Staying alive and idle keeps
// the page responsive and the report readable.
EMSCRIPTEN_KEEPALIVE void robox_web_park(void)
{
    for (;;) emscripten_thread_sleep(1000);
}

// A wasm trap unwinds to here. `e.stack` names the wasm frames, so with the
// name section intact it reads as a guest backtrace of func_<VA> symbols --
// far more precise than the r1 frame-chain walk, which only sees calls that
// bothered to spill LR.
EM_JS(void, robox_web_guarded_call, (void), {
    try {
        _robox_web_call_entry();
    } catch (e) {
        // "unwind" and ExitStatus are emscripten's normal control flow for
        // exit()/emscripten_exit_with_live_runtime, not crashes.
        if (e === 'unwind' || (e && e.name === 'ExitStatus')) throw e;

        var name = (e && e.name) ? e.name : typeof e;
        var msg  = (e && e.message) ? e.message : String(e);
        err('!!!ROBOX-CRASH-BEGIN!!!');
        err('Robox web crash: ' + name + ': ' + msg);
        if (name === 'RangeError') {
            err('(RangeError here means the JS/wasm call stack was exhausted --');
            err(' runaway guest recursion, or -sSTACK_SIZE is too small.)');
        }
        err('!!!ROBOX-CRASH-END!!!');

        // Guest register file, crashing guest address, backtrace, rings.
        // Safe to call: a trap leaves the instance fully usable.
        _robox_web_on_crash();

        err('!!!ROBOX-CRASH-BEGIN!!!');
        err('host (wasm) stack -- func_XXXXXXXX frames are guest addresses:');
        err((e && e.stack) ? e.stack : '(no stack; build with --profiling-funcs)');
        err('!!!ROBOX-CRASH-END!!!');

        _robox_web_park();   // never returns; see robox_web_park above
    }
});

/* Read a boolean flag out of the page URL: 1, 0, or -1 if absent.
 * Proxied to the main thread because `location` does not exist on this worker.
 * Used by the mod loader (?coop=1) and anything else that wants a reload-time
 * switch without a rebuild. */
int robox_web_url_flag(const char *name)
{
    return MAIN_THREAD_EM_ASM_INT({
        var n = UTF8ToString($0);
        var m = new RegExp('[?&]' + n + '=([01])').exec(location.search);
        return m ? (m[1] | 0) : -1;
    }, name);
}

void robox_web_run_guest(PPC_Func entry)
{
    g_web_entry = entry;
    robox_web_guarded_call();
}

// ---------------------------------------------------------------------------
// Hang watchdog.
//
// Runs entirely on the browser main thread. Symbolising the guest LR is a
// binary search over g_func_table_addrs, which is sorted by construction --
// the same search ppc_lookup_func does, reimplemented in JS purely so that no
// wasm call is needed from a thread that does not own the runtime.
// ---------------------------------------------------------------------------
void robox_web_watchdog_start(void)
{
    extern volatile uint32_t g_flip_count;
    extern volatile int      g_prof_in_wait;   /* sdk/robox_profiler.c */
    extern int               g_gx_key_verify;  /* sdk/gx_ogl.c */

    /* ?gxverify=1 -> double-hash every draw and report any stale memoized
     * draw key. Read from the URL rather than the environment because the
     * guest thread's getenv cannot see Module.ENV (that lives on the main
     * thread; this worker's copy is empty). Runs before the guest entry. */
    g_gx_key_verify = MAIN_THREAD_EM_ASM_INT({
        return /[?&]gxverify=1/.test(location.search) ? 1 : 0;
    });

    /* ?fps=1 -> on-screen frame counter. The desktop shortcut is F12, which a
     * browser swallows for devtools before the page sees it, so the web build
     * needs a switch that does not depend on a keypress. */
    {
        extern int g_show_fps;
        int f = robox_web_url_flag("fps");
        if (f >= 0) g_show_fps = f;
    }

    /* ?presentdiag=1 -> the once-a-second present diagnostic in
     * robox_gl_swap(). Off by default: it costs a glReadPixels and a
     * glGetError, both of which stall on the GPU, and it puts a console line
     * every second in front of every visitor. Kept because it is the thing
     * that distinguishes "we never present" from "we present a black buffer",
     * which is worth a lot the next time the canvas goes black. */
    {
        extern int g_web_present_diag;   /* sdk/video.c */
        int d = robox_web_url_flag("presentdiag");
        if (d >= 0) g_web_present_diag = d;
    }

    // NOTE: the JS body is a preprocessor macro argument, so a comma anywhere
    // outside parentheses splits it into extra arguments and the build fails
    // with a wall of "expected expression". One `var` per statement, always.
    MAIN_THREAD_EM_ASM({
        var flipPtr  = $0 >> 2;
        var lrPtr    = $1 >> 2;
        var tablePtr = $2 >> 2;
        var tableLen = $3;
        var waitPtr  = $4 >> 2;

        function symbolize(addr) {
            if (!tableLen) return '(no function table)';
            var lo = 0;
            var hi = tableLen - 1;
            var best = -1;
            while (lo <= hi) {
                var mid = (lo + hi) >> 1;
                if (HEAPU32[tablePtr + mid] <= addr) { best = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
            if (best < 0) return '(below the first recompiled function)';
            var base = HEAPU32[tablePtr + best];
            return 'func_' + base.toString(16).padStart(8, '0') +
                   '+0x' + (addr - base).toString(16);
        }

        var lastFlip = -1;
        var stalledTicks = 0;
        var reported = false;
        var started = false;
        setInterval(function () {
            // HEAPU32 is re-created whenever the heap grows, so re-read the
            // module-scope binding every tick rather than caching a view.
            var flips = HEAPU32[flipPtr];
            if (flips !== lastFlip) {
                lastFlip = flips;
                stalledTicks = 0;
                reported = false;
                started = started || flips > 0;
                return;
            }
            if (!started) return;             // still loading; not a hang
            if (++stalledTicks < 5 || reported) return;
            reported = true;
            var lr = HEAPU32[lrPtr];
            err('!!!ROBOX-CRASH-BEGIN!!!');
            err('Robox web HANG: no frame presented for ' + stalledTicks +
                's (stuck at flip #' + flips + ')');
            err('  guest address : 0x' + lr.toString(16).padStart(8, '0') +
                '  ' + symbolize(lr));
            err('  The game is running but not drawing -- an infinite loop or a');
            err('  wait that never completes, not a trap. No crash was thrown.');
            err('!!!ROBOX-CRASH-END!!!');
        }, 1000);

        // ---------------------------------------------------------------
        // Guest sampling profiler.
        //
        // The Windows build samples g_cpu.lr from a host thread; the web had
        // no equivalent, so every dropped-frame report said "(no guest
        // samples in this frame)" -- which is why the web port could only be
        // guessed at. Under PROXY_TO_PTHREAD the browser main thread is idle,
        // and g_cpu.lr lives in the SharedArrayBuffer the guest worker is
        // writing, so sampling it from here costs the guest exactly nothing.
        //
        // setInterval is clamped to ~4 ms once nested, giving ~250 Hz. That
        // is plenty: 10 seconds is ~2500 samples, and we only need to rank
        // functions, not time them.
        //
        // g_prof_in_wait is essential, not a refinement. The guest paces
        // itself by blocking in the 60 Hz frame limiter, and samples taken
        // while it is parked there are idle time, not work -- counting them
        // put the frame-end/vsync function at 71% and buried everything that
        // actually costs anything. The Windows sampler has always masked
        // these out; this one has to as well. Their ratio is also the single
        // most useful number here: it IS the frame-time headroom.
        //
        // Usage from the console:  roboxProfile()  -- or roboxProfile(20)
        var samples = {};
        var sampleCount = 0;
        var waitCount = 0;
        setInterval(function () {
            if (HEAPU32[waitPtr]) { waitCount++; return; }
            var lr = HEAPU32[lrPtr];
            if (!lr) return;
            samples[lr] = (samples[lr] || 0) + 1;
            sampleCount++;
        }, 0);

        globalThis.roboxProfile = function (seconds) {
            samples = {};
            sampleCount = 0;
            waitCount = 0;
            var secs = seconds || 10;
            err('[profile] sampling the guest for ' + secs + 's...');
            return new Promise(function (resolve) {
                setTimeout(function () {
                    // Fold raw addresses into their enclosing function.
                    var byFunc = {};
                    for (var addr in samples) {
                        var sym = symbolize(+addr).split('+')[0];
                        byFunc[sym] = (byFunc[sym] || 0) + samples[addr];
                    }
                    var ranked = Object.keys(byFunc).sort(function (a, b) {
                        return byFunc[b] - byFunc[a];
                    });
                    var total = sampleCount + waitCount;
                    var idlePct = total ? (100 * waitCount / total).toFixed(1) : '0';
                    var lines = [
                        '[profile] ' + sampleCount + ' working samples over ' + secs + 's',
                        '[profile] ' + idlePct + '% of wall time parked in the 60 Hz wait' +
                        ' (that is the headroom; 0% means every frame is late)',
                        '[profile] percentages below are of WORKING time only'
                    ];
                    for (var i = 0; i < ranked.length && i < 25; i++) {
                        var pct = (100 * byFunc[ranked[i]] / sampleCount).toFixed(1);
                        lines.push('  ' + ('     ' + pct).slice(-5) + '%  ' +
                                   byFunc[ranked[i]] + '\t' + ranked[i]);
                    }
                    var text = lines.join('\n');
                    err(text);
                    resolve(text);
                }, secs * 1000);
            });
        };
    },
    (void *)&g_flip_count,
    (void *)&g_cpu.lr,
    (void *)g_func_table_addrs,
    (int)g_func_table_count,
    (void *)&g_prof_in_wait);
}

#endif /* __EMSCRIPTEN__ */
