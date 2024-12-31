#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../include/utils.h"
#include "../include/env.h"


unsigned int ROWS, COLS;

// -------- MESSAGES ------------
char greeting_1[] = "If you're tired, wanderer, take a rest there in the lodge...";
char greeting_2[] = "Oh, yeah, and let me know if you want to leave...";
char bye[] = "Good luck on your journey, wanderer...";
// ------------------------------


// --------------- INPUT MANAGMENT ------------------

struct termios oldt, newt;

void disable_input() {
    tcgetattr(STDIN_FILENO, &oldt);  // Get the current terminal settings
    newt = oldt;  // Copy the settings

    newt.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);  // Apply the new settings
}

void enable_input() {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore the old settings
}

// ---------------------------------------------------


void render_message(char* message){
    printf(CLEAR_SCREEN);
    printf(MV_TOP_LEFT);
    size_t i = 0;
    while (message[i] != '.'){
        printf("%c", message[i]);
        fflush(stdout);
        usleep(40000);
        ++i;
    }
    while (message[i] != '\0'){
        printf("%c", message[i]);
        fflush(stdout);
        usleep(250000);
        ++i;
    }
    usleep(800000);;
}

void handle_signal(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        looping = 0;  // TODO: env var

        // pthread_mutex_destroy(&print_mutex);

        render_message(bye);
        printf(SHOW_CURSOR);
        printf(CLEAR_SCREEN);
        enable_input();
        exit(0);
    }
}

void get_sizes(unsigned int* rows, unsigned int* cols){
    struct winsize ws;

    // Get the size of the terminal
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);

    *rows = ws.ws_row;
    *cols = ws.ws_col;
}


int main() {
    // TODO: edit arguments passing
    // if (argc != 3) {
    //     printf("%d - cur number of arguments\n", argc);
    //     printf("Usage: %s <landscape_file> <message_file>\n", argv[0]);
    //     return 1;
    // }

    disable_input();
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    get_sizes(&ROWS, &COLS);
    char main_picture[ROWS][COLS];

    printf(HIDE_CURSOR);

    render_message(greeting_1);
    render_message(greeting_2);

    printf("%d, %d\n", ROWS, COLS);
    while (1){
        int a = 1;
    }

}

