# ROBOX Recompiled — Web

The browser port of [ROBOX Recompiled](https://github.com/Eggscantfly/robox-recompiled),
built with Emscripten.

## Play it

### **https://joykhloe.com/robox**

Live and fully playable in the browser. Nothing to install, nothing to supply —
the deployment carries its own data, so the first-run setup that the desktop
build shows does not appear here.

A desktop browser with WebAssembly and WebGL 2 is required, which in practice
means anything current. Give it a moment on first load: the payload is
substantial and is fetched before the game starts.

## What this repository is

The web shell, the deployment harness and the platform layer. The game itself
lives in the runtime repository and is consumed as a submodule.

## What is here

```
web_shell.html          the page the wasm build is emitted into
web_mods.cfg            mods.cfg for the web build, preloaded in place of the
                        desktop one
serve_web.py            local static server for testing a build
webdeploy/              Cloudflare Workers deployment
  stage.ps1             stages the payload for a case-sensitive host
  upload.ps1            uploads it
  worker.js             the worker
  wrangler.toml         its config
platform/
  robox_web_debug.c     crash and hang reporting. A wasm trap is a thrown JS
                        exception rather than a signal, so the desktop signal
                        handlers never fire; this catches it at the JS boundary
                        and reports the crashing guest address
```

## Getting the runtime

```sh
git submodule add https://github.com/Eggscantfly/robox-recompiled.git runtime
git submodule update --init --recursive
```

## Building

You need the Emscripten SDK, plus what the desktop build needs: a checkout of
[WiiRecomp](https://github.com/Eggscantfly/WiiRecomp) and your own copy of the
game, since the recompiled sources are generated at configure time and never
committed.

The runtime's CMake takes the shell and the mods file as paths, so point them
back here:

```sh
emcmake cmake -S runtime -B build-web \
    -DROBOX_WEB_SHELL=$PWD/web_shell.html \
    -DROBOX_WEB_MODS=$PWD/web_mods.cfg
```

## How the platform split works

The runtime keeps its `#if defined(__EMSCRIPTEN__)` blocks — twelve files carry
them, and they compile to nothing on other targets. What lives here is the
shell, the deploy harness, and the one source file that is web and nothing
else.

`robox_web_debug.c` is wholly wrapped in `#if defined(__EMSCRIPTEN__)`, so the
desktop build treated it as an empty translation unit. Moving it here changed
nothing for desktop, which was verified before the split was published.

## A note on game data

The web build preloads its data into MEMFS, so unlike desktop there is no
first-run setup screen — it compiles out.

That means a deployment necessarily carries the game's data inside its payload,
which is a meaningfully different position from the desktop build, where the
user supplies their own copy and nothing is distributed. Whoever puts a build
online owns that decision and its consequences. Nothing in this repository
contains game data, and nothing here should.

## Licence

GPL-2.0-or-later, matching the runtime. See the runtime repository for the full
text and the third-party component list.
