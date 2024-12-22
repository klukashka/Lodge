# Installing library using released packages:

* ## deb (for Debian/Ubuntu)

    1. Make sure you have a `gcc` compiler installed

    2. Download the latest release of the package
    
    3. Go to the directory where the package is located

    4. Run
    ```
    sudo dpkg -i <package-name>
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
