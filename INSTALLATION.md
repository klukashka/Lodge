# Installing Lodge

Pick one way. After any of them just run `lodge` (on Windows: `lodge.exe`).

* ## Homebrew (macOS and Linux) — recommended

    ```
    brew install klukashka/lodge/lodge
    ```

    Update to the latest release at any time, or remove it:

    ```
    brew upgrade lodge
    brew uninstall lodge
    ```

    Notes:
    - Homebrew builds Lodge from the sources of the release, so the Xcode Command Line Tools (macOS) or a C compiler
      (Linux) must be installed. `brew install` asks for them if they are missing.
    - The formula lives in the open repository [klukashka/homebrew-lodge](https://github.com/klukashka/homebrew-lodge)
      and can be read before installing. Nothing needs to be contributed to Homebrew itself: a personal “tap” is enough.
    - There is no “unidentified developer” warning, because the program is compiled on your own machine.

* ## macOS (.pkg)

    1. Download `lodge-<version>.pkg` from the [latest release](https://github.com/klukashka/Lodge/releases/latest)
       (or build it yourself: `make pkg`).

    2. Install it. The package is not signed yet, so macOS may block a double click
       (“cannot be opened because it is from an unidentified developer”). Either right-click the file → **Open**,
       or install from the terminal:
    ```
    sudo installer -pkg lodge-<version>.pkg -target /
    ```

    3. Run `lodge`.

    The package works on Apple Silicon and Intel Macs. It installs `lodge` and `lodge-bin` into `/usr/local/bin` and the
    pictures into `/usr/local/share/lodge`. To update, install the newer `.pkg` over the old one.

    To remove it:
    ```
    sudo rm /usr/local/bin/lodge /usr/local/bin/lodge-bin
    sudo rm -r /usr/local/share/lodge
    sudo pkgutil --forget com.github.klukashka.lodge
    ```

* ## Debian/Ubuntu (.deb)

    1. Download `lodge_<version>_amd64.deb` from the [latest release](https://github.com/klukashka/Lodge/releases/latest).
       The release has a 64-bit Intel/AMD package; on other architectures (for example arm64) use Homebrew or build
       the package yourself (see below).

    2. Install it (no compiler is needed, the program is already built):
    ```
    sudo apt install ./lodge_<version>_amd64.deb
    ```
    (`sudo dpkg -i <package-name>` works too.)

    3. Run `lodge`. To update, install the newer package the same way. To remove it: `sudo apt remove lodge`.

* ## Windows 10 and 11 (.zip)

    1. Download `lodge-<version>-windows-x64.zip` from the
       [latest release](https://github.com/klukashka/Lodge/releases/latest) and unpack it anywhere
       (right-click → **Extract All**).

    2. Open the unpacked `lodge` folder and run `lodge.exe`: double click it, or type `.\lodge.exe` in Windows
       Terminal or PowerShell. Keep the `texts` folder next to the program.

    3. The program is not signed, so SmartScreen may stop it the first time: **More info → Run anyway**.

    To type just `lodge` from anywhere, add the folder to your `Path` (Settings → search for “environment variables”).
    To update, replace the folder with the newer one; to remove Lodge, delete the folder. It writes nothing else on the
    computer.

* ## From the sources

    ```
    git clone https://github.com/klukashka/Lodge.git && cd Lodge
    make
    sudo make install
    ```

    `make install` puts everything under `/usr/local` (change it with `PREFIX=...`); `sudo make uninstall` removes it.

# How Lodge behaves

- `lodge` runs right in your current terminal and takes over its whole screen, like `sl`, and it fits any size from
  74×30 up. In a smaller window it asks you to enlarge it and starts by itself.
- All keys, the mouse and the scroll wheel are ignored, except **Ctrl+C**: it prints the farewell message and returns you
  to the shell; press it twice to leave at once. (Terminal.app reserves Cmd+C for “Copy”, so it never reaches the
  program.)
- Prefer a separate window? Run `LODGE_NEW_WINDOW=1 lodge`: it opens a new full-screen window and closes it when you
  quit. On a Mac, going full screen needs the Accessibility permission for “System Events”.
- Other options (time of day, wind, a fast clock) are described in the [README](README.md).

# Building your own packages

* ## macOS (.pkg)

    Requires the Xcode Command Line Tools. From the repository root run `make pkg`;
    `lodge-<version>.pkg` appears in the repository root. It is a universal (Apple Silicon + Intel) package.

* ## Debian/Ubuntu (.deb)

    1. Make sure you have a C compiler (`build-essential`) and `debhelper` installed.

    2. Clone the repository:
    ```
    git clone https://github.com/klukashka/Lodge.git && cd Lodge
    ```

    3. Run the following in the repository root:
    ```
    make deb
    ```

    4. The built `lodge_<version>_<arch>.deb` appears in the repository root. Install it as described above.

* ## Windows (.zip)

    Built on macOS or Linux with mingw-w64 (`brew install mingw-w64`, or `sudo apt install gcc-mingw-w64-x86-64 zip`):
    run `make windows`, and `lodge-<version>-windows-x64.zip` appears in the repository root. The `windows` GitHub
    workflow builds it the same way and runs it on Windows.

# For maintainers: publishing a release

The version lives in the `VERSION` file and is used by the Makefile, the `.deb`, the `.pkg` and the formula.

1. Raise the number in `VERSION`, add an entry to `deb/debian/changelog`, commit.
2. Tag it and push the tag: `git tag v<version> && git push origin v<version>`.
   The GitHub workflow checks that the tag matches `VERSION`, builds the `.pkg`, the `.deb` and the Windows `.zip`,
   tries the program on Windows, attaches them to the
   release and updates the Homebrew tap.
3. One-time setup of the tap:
   - create an empty public repository `klukashka/homebrew-lodge` (Homebrew requires the `homebrew-` prefix);
   - create a personal access token (fine-grained, write access to *Contents* of that repository only);
   - add it to the Lodge repository as the secret `TAP_TOKEN` (Settings → Secrets and variables → Actions).

   `Formula/lodge.rb` in this repository is the template: on every tag the workflow copies it to the tap and fills in
   the URL and the checksum of the tagged source archive. Without the secret the tap step is skipped.
4. Check the result in a clean shell: `brew install klukashka/lodge/lodge && lodge`.
