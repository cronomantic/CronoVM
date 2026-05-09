#include "cvm_alloc.h"

int alloc_sum(int n) {
    int *arr = (int *)cvm_malloc(n * (int)sizeof(int));
    if (!arr) return -1;
    for (int i = 0; i < n; i++) arr[i] = i + 1;
    int s = 0;
    for (int i = 0; i < n; i++) s += arr[i];
    cvm_free(arr);
    return s;
}
