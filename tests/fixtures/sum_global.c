int data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

int sum_global(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += data[i];
    }
    return s;
}
