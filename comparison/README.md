# The counter, three ways

`examples/apps/counter-jsx` (a title, a count and three buttons: `-`, `+`,
`Reset`; UP/DOWN move focus, SELECT presses) written by hand for the two
existing ways to program a Pebble Time 2, so the Gea build has something to be
measured against. Both draw the same screen and handle the same buttons.

| | `.pbw` | heap in use |
| --- | --- | --- |
| [`c-counter`](c-counter) — the Pebble C SDK | 6.8 KB | 0.3 KB |
| Gea, compiled UI (`@geastack/pebble`) | 19.5 KB | 1.3 KB at peak |
| [`alloy-counter`](alloy-counter) — Alloy (Moddable XS + Poco) | 17 KB | 8.6 KB |

Each is an ordinary SDK project:

```sh
cd comparison/c-counter
pebble build
pebble install --emulator emery        # or --phone <ip>
```

The Alloy project needs an SDK with Alloy support (`projectType: moddable`) and
runs on `emery` and `gabbro` only.
