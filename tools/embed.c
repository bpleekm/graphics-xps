#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input) { perror(argv[1]); return 1; }
    FILE *output = fopen(argv[2], "w");
    if (!output) { perror(argv[2]); fclose(input); return 1; }
    fputs("/* Generated from kernels/gemm.cl; do not edit. */\n"
          "static const unsigned char gx_kernel_source[] = {\n", output);
    unsigned count = 0;
    int byte;
    while ((byte = fgetc(input)) != EOF) {
        fprintf(output, "%u,", (unsigned)byte);
        if (++count % 24 == 0) fputc('\n', output);
    }
    fputs("0};\n", output);
    int failed = ferror(input) || ferror(output);
    if (fclose(input)) failed = 1;
    if (fclose(output)) failed = 1;
    return failed;
}
