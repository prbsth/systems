#define NUM_ELEMENTS 32768
#define NUM_ITERATIONS 100000
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
unsigned int arr[NUM_ELEMENTS];
unsigned int sum = 0;
int main(){
    for (unsigned int i = 0; i < NUM_ITERATIONS; ++i) {
        unsigned int index = (i * 8192) % NUM_ELEMENTS;
        arr[index] = i + sum;
        printf("%i ", index);
        sum += i;
    }
}