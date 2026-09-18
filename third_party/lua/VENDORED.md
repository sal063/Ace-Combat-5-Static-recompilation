# Lua 5.4.9, vendored

Source:    https://www.lua.org/ftp/lua-5.4.9.tar.gz
Retrieved: 2026-09-16

    sha256  2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6

That digest is the one lua.org publishes on its download listing, and it
matches the tarball as downloaded -- checked, not assumed.

## What is here

`src/` only, minus `lua.c` and `luac.c`: those are the standalone interpreter
and the compiler, and each has a `main()` that would collide at link time.
32 .c and 27 .h files, unmodified.

## Licence

Lua is released under the MIT licence. The terms are in `LICENSE.html`
(the distribution's `doc/readme.html`), and the copyright notice is also
embedded at the end of `lua.h`.

The `README` file copied from the tarball root is a 151-byte pointer to the
documentation and does **not** contain the licence; `LICENSE.html` does.
