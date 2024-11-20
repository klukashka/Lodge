#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../include/landscape.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

char lines[LINES][LINE_LENGTH];
char message[1024][LINE_LENGTH];
char* landscape_file;
char* message_file;
const int thr_num = 2;
int stop_looping = 0;
pthread_t tid[2];
pthread_mutex_t print_mutex;

// Function to read the file and store its lines
int read_file_lines(const char *landscape_file, char lines[][LINE_LENGTH], int max_lines) {
    FILE *file = fopen(landscape_file, "r");
    // printf("%c", *landscape_file);
    if (file == NULL) {
        perror("Error opening file");
        exit(1);
    }

    int line_count = 0;
    while (fgets(lines[line_count], LINE_LENGTH, file) != NULL) {
        // Remove newline character at the end of each line
        size_t len = strlen(lines[line_count]);
        if (len > 0 && lines[line_count][len - 1] == '\n') {
            lines[line_count][len - 1] = '\0';
        }

        ++line_count;
        if (line_count >= max_lines) {
            break;
        }
    }

    fclose(file);
    return line_count;
}

void* render_smoke(){
    int swap_flag = 0; 
    while (!stop_looping) {
        pthread_mutex_lock(&print_mutex);
        for (int i = PIC_END; i >= SMOKE_BEGIN; --i){
            printf("\033[A");
        }

        if (swap_flag) {
            for (int i = SMOKE_BEGIN1; i <= SMOKE_END1; ++i) {
                printf("\r%s\n", lines[i]);
            }
        } else {
            for (int i = SMOKE_BEGIN; i <= SMOKE_END; ++i) {
                printf("\r%s\n", lines[i]);
            }
        }

        for (int i = SMOKE_END; i < PIC_END; ++i){
            printf("\033[B");
        }


        swap_flag = !swap_flag;
        pthread_mutex_unlock(&print_mutex);
        usleep(1100000);
        fflush(stdout);
    }

    return NULL;
}

void* render_river(){
    int river_size = RIVER_END - RIVER_BEGIN + 1;
    int pos[river_size];
    int accel[river_size];
    for (int i = 0; i < river_size; ++i){
        accel[i] = min(i, river_size - i - 1) + 1;
    }
    while (!stop_looping){
        pthread_mutex_lock(&print_mutex);
        for (int i = PIC_END; i >= RIVER_BEGIN; --i){
            printf("\033[A");
        }

        for (int row = RIVER_BEGIN; row <= RIVER_END; ++row){
            for (int i = pos[row - RIVER_BEGIN]; i < LINE_LENGTH; ++i){
                printf("%c", lines[row][i]);
            }
            for (int i = 0; i < pos[row - RIVER_BEGIN]; ++i){
                printf("%c", lines[row][i]);
            }
            printf("\n");
        }

        for (int i = RIVER_END; i < PIC_END; ++i){
            printf("\033[B");
        }

        for (int i = 0; i < river_size; ++i){
            pos[i] += accel[i];
            pos[i] = pos[i] % LINE_LENGTH;
        }
        fflush(stdout);
        pthread_mutex_unlock(&print_mutex);
        usleep(500000);
    }

    return NULL;
}

void render_message(int total_lines){
    printf("\033[2J\033[H");
    for (int i = 0; i < total_lines; ++i) {
        for (int j = 0; j < LINE_LENGTH; ++j){
            printf("%c", message[i][j]); // print a letter
            fflush(stdout);
            usleep(40000);

        }
        usleep(110000);
        // printf("\r");
        printf("\033[2K\r");
    }
    usleep(800000);

}

void render_bye(){
    printf("\033[2J\033[H\r");
    char bye[] = "Good luck on your journey, wanderer...";
    size_t i = 0;
    while (bye[i] != '.'){
        printf("%c", bye[i]);
        fflush(stdout);
        usleep(35000);
        ++i;
    }
    while (bye[i] != '\0'){
        printf("%c", bye[i]);
        fflush(stdout);
        usleep(250000);
        ++i;
    }
    usleep(400000);
}

// Function to render the file with lines swapping in place like a loading screen
void render_landscape() {
    printf("\033[2J\033[H");
    for (int i = PIC_BEGIN; i <= PIC_END; ++i) {
        printf("%s\n", lines[i]);
    }
}

void handle_signal(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        stop_looping = 1;

        pthread_mutex_destroy(&print_mutex);

        render_bye();
        printf("\033[?25h"); // show cursor
        printf("\033[2J\033[H"); // clear the screen
        exit(0);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("%d\n", argc);
        printf("Usage: %s <landscape_file>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    landscape_file = argv[1];
    printf("%c", *landscape_file);
    message_file = argv[2];

    int total_land_lines = read_file_lines(landscape_file, lines, LINES);
    int total_message_lines = read_file_lines(message_file, message, LINES);

    if (total_land_lines == 0) {
        printf("The file is empty or could not be read.\n");
        return 1;
    }

    if (pthread_mutex_init(&print_mutex, NULL) != 0) {
        fprintf(stderr, "Mutex init failed\n");
        return 1;
    }

    printf("\033[?25l");
    render_message(total_message_lines);
    render_landscape();


    if (pthread_create(&tid[0], NULL, render_smoke, NULL) != 0){
        printf("Error: failed to create a thread");
        return 1;
    }
    if (pthread_create(&tid[1], NULL, render_river, NULL) != 0){
        printf("Error: failed to create a thread");
        return 1;
    }

    for (int i = 0; i < thr_num; ++i){
        if (pthread_join(tid[i], NULL) != 0){
            printf("Error: failed to join a thread");
            exit(1);
        }
    }
}