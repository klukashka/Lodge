# Installation

1. Make sure you have a `gcc` compiler. If you don't, install it using probably

```sudo apt install gcc```

2. Clone the repository to any folder you want (it is a temporary action for creating `.deb` file)

```git clone https://github.com/klukashka/Lodge.git```

3. Run the following to build `Lodge.deb package`

```dpkg-deb --build Lodge```

4. Run the following to install `Lodge.deb`

```sudo dpkg -i Lodge.deb```

5. Take a rest after coding a lot with running

```lodge```
