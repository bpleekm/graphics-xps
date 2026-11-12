#include "gx_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc == 1) { gx_usage(stdout); return 0; }
    gx_config config;
    char error[GX_ERROR_SIZE];
    if (gx_config_parse(argc, argv, &config, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); return 2;
    }
    if (config.help) { gx_usage(stdout); return 0; }
    if (config.version) { puts("graphics-xps " GX_VERSION); return 0; }
    if (config.output) {
        int descriptor = open(config.output, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0) {
            fprintf(stderr, "Cannot reserve new report '%s': %s\n", config.output, strerror(errno));
            return 2;
        }
        close(descriptor);
    }
    gx_report report;
    gx_report_init(&report);
    gx_install_signals();
    if (gx_report_save(&report, &config, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
    int code = gx_run(&config, &report);
    report.peak_rss_bytes = gx_peak_rss();
    if (gx_report_save(&report, &config, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
    printf("Status: %s | process peak RSS %.2f MiB (separate from GPU data budget)\n",
           report.status, report.peak_rss_bytes / 1048576.0);
    if (report.error[0]) fprintf(stderr, "%s\n", report.error);
    if (config.output) printf("JSON: %s\n", config.output);
    return code;
}
