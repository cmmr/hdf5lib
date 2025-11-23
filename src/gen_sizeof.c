#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>

/* Helper for Endianness */
int is_little_endian(void) {
    int x = 1;
    return *(char *)&x;
}

int main(void) {
    printf("#ifndef H5_SIZEOF_H\n");
    printf("#define H5_SIZEOF_H\n\n");

    printf("/* Generated Type Sizes */\n");
    printf("#define H5_SIZEOF_CHAR %zu\n", sizeof(char));
    printf("#define H5_SIZEOF_SHORT %zu\n", sizeof(short));
    printf("#define H5_SIZEOF_INT %zu\n", sizeof(int));
    printf("#define H5_SIZEOF_UNSIGNED %zu\n", sizeof(unsigned));
    printf("#define H5_SIZEOF_LONG %zu\n", sizeof(long));
    printf("#define H5_SIZEOF_LONG_LONG %zu\n", sizeof(long long));
    printf("#define H5_SIZEOF_FLOAT %zu\n", sizeof(float));
    printf("#define H5_SIZEOF_DOUBLE %zu\n", sizeof(double));
    printf("#define H5_SIZEOF_SIZE_T %zu\n", sizeof(size_t));
    printf("#define H5_SIZEOF_SSIZE_T %zu\n", sizeof(ssize_t));
    printf("#define H5_SIZEOF_PTRDIFF_T %zu\n", sizeof(ptrdiff_t));
    printf("#define H5_SIZEOF_TIME_T %zu\n", sizeof(time_t));
    
    /* Critical for LFS (Large File Support) */
    printf("#define H5_SIZEOF_OFF_T %zu\n", sizeof(off_t));

    /* Complex types (R uses C99 complex) */
    printf("#define H5_SIZEOF_FLOAT_COMPLEX %zu\n", sizeof(float _Complex));
    printf("#define H5_SIZEOF_DOUBLE_COMPLEX %zu\n", sizeof(double _Complex));

    /* Endianness */
    if (is_little_endian()) {
        printf("#undef WORDS_BIGENDIAN\n");
    } else {
        printf("#define WORDS_BIGENDIAN 1\n");
    }
    
    printf("\n#endif\n");
    return 0;
}
