# node-red-contrib-libgpiod

[![npm](https://img.shields.io/npm/v/node-red-contrib-libgpiod?style=plastic)](https://www.npmjs.com/package/node-red-contrib-libgpiod)

A set of input and output nodes for controlling General Purpose Input and Outputs (GPIOs) though libgpiod (ioctl)

### Requirements

- [node-libgpiod](https://github.com/sombriks/node-libgpiod) (native nodejs bindings for libgpiod)
- `gpiomon` from libgpiod tools must be installed and available on `PATH` to use the `gpio-watch` node

### Install

```
cd ~/.node-red
npm install node-red-contrib-libgpiod
```

### Usage

in nodes configuration choose device and correct pin number  
for output inject msg.payload = true/1 for high state and false/0 for low state  
for input inject any value to trigger reading

### gpio-watch

`gpio-watch` is a source node that uses libgpiod edge monitoring through `gpiomon` and emits a message whenever the watched line changes.

Available watch options:

- `bias`: `as-is`, `pullup`, `pulldown`, `floating`
- `edge`: `both`, `rising`, `falling`
- `debounce`: software debounce in milliseconds

Default watch behavior:

- bias: `as-is`
- debounce: `0`
- edge: `both`

Each watch message contains:

- `payload`: inferred line value after the edge (`1` on rising, `0` on falling)
- `edge`: `rising` or `falling`
- `timestamp`: `{ seconds, nanoseconds }` from libgpiod
- `timestampMs`: timestamp converted to milliseconds
- `pin`, `device`, `mode`, `bias`, `debounce`

`gpio-in` remains the manual read node. Use `gpio-watch` when you want the node to push messages on GPIO changes without an input trigger.

### Tested on

- [LTPPxG2](https://tibbo.com/store/tps/ltpp3g2.html) with sp7021 SoC (32 bits, 512MB ram) running Yocto
