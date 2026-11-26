// The Windows side of Lodge: the console, the keyboard, the clock and Ctrl+C. Included by lodge.c only, in place of
// the POSIX functions of the same names; it uses the flags declared there (stop_looping, hangup, ...).
// Needs a console that understands ANSI escape sequences: Windows 10 or later (Windows Terminal, PowerShell, cmd).
#ifndef LODGE_PLATFORM_WIN_H
#define LODGE_PLATFORM_WIN_H

#include <fcntl.h>
#include <io.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

#define localtime_r(timep, result) (localtime_s((result), (timep)) == 0 ? (result) : NULL)

static DWORD g_old_in_mode = 0, g_old_out_mode = 0;
static int g_in_saved = 0, g_out_saved = 0;

// Switches the output console to ANSI escape sequences. Returns 0 when there is no console that can do it.
static int console_prepare(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(out, &g_old_out_mode)) {
        return 0;  // redirected, or a terminal that is not a Windows console (for example mintty without winpty)
    }
    DWORD mode = g_old_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(out, mode | DISABLE_NEWLINE_AUTO_RETURN) && !SetConsoleMode(out, mode)) {
        return 0;
    }
    g_out_saved = 1;
    _setmode(_fileno(stdout), _O_BINARY);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);  // a frame goes out in one piece (fb_flush flushes it)
    return 1;
}

// Only Ctrl+C (and Ctrl+Break) reaches the program: no echo, no line editing, and no "QuickEdit" selection, which
// would freeze the picture on a mouse click.
static void disable_input(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(in, &g_old_in_mode)) {
        return;  // stdin is not a console
    }
    g_in_saved = 1;
    DWORD mode = g_old_in_mode & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_QUICK_EDIT_MODE);
    SetConsoleMode(in, mode | ENABLE_PROCESSED_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT);
}

// Restores the console; also drops anything typed meanwhile so it does not reach the shell.
static void restore_input(void) {
    if (g_in_saved) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        FlushConsoleInputBuffer(in);
        SetConsoleMode(in, g_old_in_mode | ENABLE_EXTENDED_FLAGS);
        g_in_saved = 0;
    }
    if (g_out_saved) {
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_old_out_mode);
        g_out_saved = 0;
    }
}

static void drain_input(void) {
    if (g_in_saved) {
        FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    }
}

// Milliseconds since the first call (a `long` is 32 bits here, so the time since boot would overflow it).
static long now_ms(void) {
    static ULONGLONG start = 0;
    ULONGLONG t = GetTickCount64();
    if (start == 0) {
        start = t;
    }
    return (long)(t - start);
}

// The size of the visible part of the console window. Returns 0 when it is unknown.
static int term_size(int *rows, int *cols) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return 0;
    }
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    return *rows > 0 && *cols > 0;
}

// Runs on its own thread; like the POSIX signal handler, it only raises flags.
static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        if (++interrupts >= 2) {
            fast_exit = 1;
        }
        stop_looping = 1;
    } else {  // the window is closing, log off, shutdown: nobody to say goodbye to
        hangup = 1;
        stop_looping = 1;
    }
    return TRUE;
}

static void install_handlers(void) {
    SetConsoleCtrlHandler(NULL, FALSE);  // in case the parent process switched Ctrl+C off
    SetConsoleCtrlHandler(console_handler, TRUE);
}

// Goes to the folder of lodge.exe, so that the pictures next to it can be opened by relative paths.
static int enter_exe_dir(void) {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, 32768);
    if (n == 0 || n >= 32768) {
        return 0;
    }
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') {
        --n;
    }
    path[n] = L'\0';
    return SetCurrentDirectoryW(path) != 0;
}

#endif
