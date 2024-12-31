#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "trial.h"

char greeting_1[] = "If you're tired, wanderer, take a rest there in the lodge...";

void render_message(char* message){
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

int main(){
    render_message(greeting_1);
}