# GeaStack for Pebble

Build a [Gea](https://geastack.com) TSX app into an ordinary Pebble app for the
**Pebble Time 2** (`emery`: 200×228, 64 colours, touch, four buttons). The
components are compiled to native ARM code by geatsc and linked with a small
Pebble backend. There is no JavaScript engine on the watch and no firmware
change: the `.pbw` installs through the Pebble phone app or the SDK emulator
like any other app.

```tsx
import { ReactiveComponent, mount } from '@geastack/core'
import './App.css'

class App extends ReactiveComponent {
  count = 0

  template() {
    return (
      <div class="counter-app">
        <span class="counter-title">Counter</span>
        <span class="counter-value">{this.count}</span>
        <div class="counter-minus-button" onClick={() => this.count--}>-</div>
        <div class="counter-plus-button" onClick={() => this.count++}>+</div>
      </div>
    )
  }
}

mount(App)
```

An app opts in with `"pebble": true` under `gea.targets` in its `package.json`
and a dependency on `@geastack/pebble`. Then:

```sh
gea build --target pebble                        # -> dist/pebble/<app-id>.pbw
gea run   --target pebble                        # build and install on the emery emulator
gea run   --target pebble --phone 192.168.1.23   # install on a watch through its phone
```

UP/DOWN move focus between controls, SELECT presses, BACK exits, and a tap hits
the element under the finger.

## What runs

These are earlier device measurements; sizes vary with the compiler version.

| app | program | notes |
| --- | --- | --- |
| `counter-jsx` | 9.9 KB, 1.3 KB peak heap | UI compiled at build time: the page is constant draw tables |
| `bouncing-balls-jsx` | 49 KB | 64 reactively styled elements animated by `requestAnimationFrame`, ~26 FPS on the watch |

A Pebble app gets 128 KB for code and heap together, and at most 64 KB of app
image. A page whose layout is fixed is laid out during the build and ships as
tables; anything dynamic (lists, conditions, child components) runs on a small
flexbox engine that paints with PebbleOS graphics and system fonts.

## Repository

- [`packages/geastack-pebble`](packages/geastack-pebble) — `@geastack/pebble`,
  the target: the build script, the app shell, the runtime backend, the
  build-time UI compiler plugin and the size/heap tools. Its README covers how
  it works and what keeps it small.
- [`comparison`](comparison) — the same counter written against the Pebble C
  SDK and in Alloy, with sizes side by side.

## Getting started

Use **@geastack/cli 0.1.85 or newer**. In your existing Gea app, install:

```sh
npm install @geastack/core@^0.1.26 @geastack/pebble@^0.1.0
npm install --save-dev @geastack/cli@^0.1.85
```

Preserve your existing manifest and add `"pebble": true` to `gea.targets`.
The CLI forwards `gea.entry`, including custom paths such as `src/watch.tsx`.
Use `npx gea` for the commands above when the CLI is installed locally.

The [complete quickstart](https://github.com/geastack/cli/blob/main/docs/PEBBLE-QUICKSTART.md)
covers the SDK, manifest, emulator, phone developer connection, logs and
limits. Only Pebble Time 2 (`emery`) is supported; use macOS or Linux with Bash.
The package installs its native framework dependencies automatically.

## Requirements

- The Pebble SDK: `uv tool install --python 3.13 pebble-tool`, then
  `pebble sdk install latest`.
- `@geastack/core` ≥ 0.1.26 and the geatsc compiler.
- Optionally LLVM (`brew install llvm lld`): the program is then built `-Oz`
  with full LTO, noticeably smaller than with the SDK's GCC.

## License

Apache-2.0 (see `LICENSE`). You can ship closed-source products built on it.
Contact [contact@geastack.com](mailto:contact@geastack.com) for commercial
terms, support and hosted builds.
