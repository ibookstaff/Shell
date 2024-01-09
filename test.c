#include <stdio.h>
#include <unistd.h>


int main(){
    int count = 0;
    while(1){
        printf("%d: A\n", count);
        sleep(1);
        count++;
    }
}