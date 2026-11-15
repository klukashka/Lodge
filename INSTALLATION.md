# Installing library using released packages:

* ## deb (for Debian/Ubuntu)

    1. Make sure you have a `gcc` compiler installed

    2. Download the latest release of the package
    
    3. Go to the directory where the package is located

    4. Run
    ```
    sudo dpkg -i <package-name>
    ```

* ## macOS (.pkg)

    1. Download `lodge-<version>.pkg` from the latest release (or build it: `make pkg VERSION=<version>`)

    2. Install it. The package is not signed yet, so macOS may block a double-click
       ("cannot be opened because it is from an unidentified developer"). Either right-click the file → **Open**,
       or install from the terminal:
    ```
    sudo installer -pkg lodge-<version>.pkg -target /
    ```

    3. Run `lodge`

* ## macOS and Linux (Homebrew)

    ```
    brew install klukashka/lodge/lodge
    ```

    Then run:

    ```
    lodge
    ```

    Update to the latest release at any time, or remove it:

    ```
    brew upgrade lodge
    brew uninstall lodge
    ```

    Notes:
    - Homebrew builds Lodge from the sources of the release, so the Xcode Command Line Tools (macOS) or a C compiler
      (Linux) must be installed. `brew install` asks for them if they are missing.
    - `lodge` runs right in your current terminal and takes over its whole screen, like `sl`.
    - All keys are ignored except **Ctrl+C**: it prints the farewell message and returns you to the shell; press it
      twice to leave at once. (Terminal.app reserves Cmd+C for “Copy”, so it never reaches the program.)
    - Prefer a separate window? Run `LODGE_NEW_WINDOW=1 lodge`: it opens a new full-screen Terminal window and closes
      it when you quit. Switching to full screen needs the Accessibility permission for “System Events”.

* ## Build and install locally (without Homebrew)

    ```
    make
    sudo make install
    ```

    Then run `lodge`.

* ## For maintainers: publishing a release

    1. Raise the number in `VERSION`, add an entry to `deb/debian/changelog`, commit.
    2. Tag it and push the tag: `git tag v<version> && git push origin v<version>`.
       The GitHub workflow builds the `.pkg` and the `.deb`, attaches them to the release and updates the Homebrew tap.
    3. One-time setup of the tap:
       - create an empty public repository `klukashka/homebrew-lodge` (Homebrew requires the `homebrew-` prefix);
       - create a personal access token (fine-grained, write access to *Contents* of that repository only);
       - add it to the Lodge repository as the secret `TAP_TOKEN` (Settings → Secrets and variables → Actions).

       `Formula/lodge.rb` in this repository is the template: on every tag the workflow copies it to the tap and fills
       in the URL and the checksum of the tagged source archive. Without the secret the tap step is skipped.


# Building your own package from the existing one:

* ## macOS (.pkg)

    Requires Xcode Command Line Tools. From the repository root run `make pkg VERSION=<version>`;
    `lodge-<version>.pkg` appears in the repository root.

* ## deb (Debian/Ubuntu)

    1. Make sure you have a `gcc` compiler and `debhelper` installed

    2. Clone the repository to any folder you want

    ```
    git clone https://github.com/klukashka/Lodge.git
    ```

    3. Run the following in the repository root to build a package

    ```
    make deb
    ```

    4. The built `lodge_<version>_<arch>.deb` will appear in the repository root. To install it follow the *3rd* and *4th* steps of the guide above

* ## Version

    The version lives in the `VERSION` file and is used by the Makefile, the `.deb` and the `.pkg`.
    To release: edit `VERSION`, commit, tag `v<version>` and push the tag — the GitHub workflow builds both packages and attaches them to the release.
