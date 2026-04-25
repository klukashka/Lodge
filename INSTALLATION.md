# Installing library using released packages:

* ## deb (for Debian/Ubuntu)

    1. Make sure you have a `gcc` compiler installed

    2. Download the latest release of the package
    
    3. Go to the directory where the package is located

    4. Run
    ```
    sudo dpkg -i <package-name>
    ```

* ## macOS (Homebrew)

    Lodge is packaged for Linux via `.deb`. The macOS equivalent is a Homebrew formula that:
    - installs the compiled binary
    - installs the bundled text assets into `share/lodge/texts`
    - exposes `lodge` as the user-facing command (opens Terminal and runs Lodge)

    1. Install via Homebrew (recommended: via a tap)

    Homebrew requires third-party formulae to live in a **tap** (a separate Git repo), so you can’t `brew install ./Formula/lodge.rb` directly.

    As the maintainer, you publish a tap once, then users install with:

    ```
    brew tap <user-or-org>/lodge
    brew install lodge
    ```

    Then run:

    ```
    lodge
    ```

    Notes:
    - The fullscreen behavior is implemented by sending the standard macOS fullscreen shortcut (Ctrl+Cmd+F) to Terminal.
    - On some systems macOS may ask for Accessibility permission to allow “System Events” to send keystrokes. If you deny it, Lodge will still open in Terminal, just not forced fullscreen.

    2. Build and install locally (without Homebrew):

    ```
    make
    sudo make install
    ```

    Then run:

    ```
    lodge
    ```

    3. Publish via Homebrew (maintainers)

    Create a tap repository (common naming: `homebrew-lodge`), add the formula, and push it:

    - Create the tap repo on GitHub: `<user-or-org>/homebrew-lodge`
    - Locally:

    ```
    brew tap-new <user-or-org>/lodge
    ```

    - Copy `Formula/lodge.rb` into the newly created tap repo as `Formula/lodge.rb`
    - Commit + push the tap repo

    Notes:
    - The **tap’s** `Formula/lodge.rb` should use a **GitHub release tarball** `url` + `sha256` (stable). In this repo, `Formula/lodge.rb` is a template: update the version + SHA when you cut a new release.
    - You can also keep `head` for `brew install --HEAD lodge` (builds from `main`).

    Users will then be able to install with:

    ```
    brew install <tap>/lodge
    ```
    
# Building your own package from the existing one:

* ## deb (Debian/Ubuntu)

    1. Make sure you have a `gcc` compiler and `debhelper` installed

    2. Clone the repository to any folder you want

    ```
    git clone https://github.com/klukashka/Lodge.git
    ```

    3. Go into **deb/** folder of the project

    4. Run the following to build a package

    ```
    dpkg-buildpackage -us -uc
    ```

    5. The built package will appear in the parent directory. To install it follow the *3rd* and *4th* steps of the guide above
