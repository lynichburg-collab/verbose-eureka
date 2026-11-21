# Trying MTplatitudes in your browser

You can run the OS without installing anything, from any computer with a web
browser, using a GitHub Codespace: a Linux machine on GitHub's servers with a
terminal in your browser. MTplatitudes boots inside it in a virtual machine.

## Start

1. On the repository's page, click **Code** › **Codespaces** › **Create
   codespace on main**. The first start takes a few minutes while it installs
   QEMU; later starts are quick.
2. In the terminal at the bottom, run:

   ```sh
   ./forge try
   ```

   This downloads the newest build that passed GitHub's checks and boots it.
   Nothing is compiled.
3. The first screen asks you to press Enter and create an account (any name
   and password: it only lives inside this test machine and disappears when
   you quit).
4. To quit: **Ctrl-A**, then **X**. Or type `elevate poweroff`.

## Things to know

- **It's the serial console**, a plain text terminal. You see what you would
  see on the laptop's screen, except the colors come from your browser's
  terminal theme rather than the console palette, and Ctrl+Alt+F2-style
  switching between screens isn't available (there is one terminal).
- **Everything resets** when you quit, just like on real hardware for now.
- **Free usage:** personal GitHub accounts include 120 core-hours of
  Codespaces a month: about 60 hours on the default 2-core machine. A
  codespace stops by itself after 30 idle minutes; stop it yourself from
  github.com/codespaces when you're done, and delete old ones you don't need.
- **To test a change before it's pushed**, build it in the codespace instead:
  `./forge` (the first build compiles the kernel: tens of minutes on 2 cores),
  then `./forge run`.
