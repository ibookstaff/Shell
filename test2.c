#include <stdio.h>
#include <unistd.h>


int main(){
    int count = 0;
    while(1){
        printf("%d: B\n", count);
        sleep(2);
        count++;
    }
}
