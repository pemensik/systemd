/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "alloc-util.h"
#include "binfmt-util.h"
#include "conf-files.h"
#include "def.h"
#include "fd-util.h"
#include "fileio.h"
#include "log.h"
#include "main-func.h"
#include "pager.h"
#include "path-util.h"
#include "pretty-print.h"
#include "string-util.h"
#include "strv.h"

static bool arg_cat_config = false;
static PagerFlags arg_pager_flags = 0;
static bool arg_unregister = false;

static int delete_rule(const char *rule) {
        _cleanup_free_ char *x = NULL, *fn = NULL;
        char *e;

        assert(rule);
        assert(rule[0]);

        e = strchrnul(rule + 1, rule[0]);
        x = strndup(rule + 1, e - rule - 1);
        if (!x)
                return log_oom();

        if (!filename_is_valid(x) ||
            STR_IN_SET(x, "register", "status"))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Rule file name '%s' is not valid, refusing.", x);

        fn = path_join("/proc/sys/fs/binfmt_misc", x);
        if (!fn)
                return log_oom();

        return write_string_file(fn, "-1", WRITE_STRING_FILE_DISABLE_BUFFER);
}

static int apply_rule(const char *rule) {
        int r;

        (void) delete_rule(rule);

        r = write_string_file("/proc/sys/fs/binfmt_misc/register", rule, WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to add binary format: %m");

        return 0;
}

static int apply_file(const char *path, bool ignore_enoent) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *pp = NULL;
        int r;

        assert(path);

        r = search_and_fopen(path, "re", NULL, (const char**) CONF_PATHS_STRV("binfmt.d"), &f, &pp);
        if (r < 0) {
                if (ignore_enoent && r == -ENOENT)
                        return 0;

                return log_error_errno(r, "Failed to open file '%s': %m", path);
        }

        log_debug("apply: %s", pp);
        for (;;) {
                _cleanup_free_ char *line = NULL;
                char *p;
                int k;

                k = read_line(f, LONG_LINE_MAX, &line);
                if (k < 0)
                        return log_error_errno(k, "Failed to read file '%s': %m", pp);
                if (k == 0)
                        break;

                p = strstrip(line);
                if (isempty(p))
                        continue;
                if (strchr(COMMENTS, p[0]))
                        continue;

                k = apply_rule(p);
                if (k < 0 && r == 0)
                        r = k;
        }

        return r;
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-binfmt.service", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n\n"
               "Registers binary formats with the kernel.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "     --cat-config       Show configuration files\n"
               "     --no-pager         Do not pipe output into a pager\n"
               "     --unregister       Unregister all existing entries\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_CAT_CONFIG,
                ARG_NO_PAGER,
                ARG_UNREGISTER,
        };

        static const struct option options[] = {
                { "help",       no_argument, NULL, 'h'            },
                { "version",    no_argument, NULL, ARG_VERSION    },
                { "cat-config", no_argument, NULL, ARG_CAT_CONFIG },
                { "no-pager",   no_argument, NULL, ARG_NO_PAGER   },
                { "unregister", no_argument, NULL, ARG_UNREGISTER },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_CAT_CONFIG:
                        arg_cat_config = true;
                        break;

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case ARG_UNREGISTER:
                        arg_unregister = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if ((arg_unregister || arg_cat_config) && argc > optind)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Positional arguments are not allowed with --cat-config or --unregister");

        return 1;
}

static int run(int argc, char *argv[]) {
        int r, k;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

        log_setup();

        umask(0022);

        r = 0;

        if (arg_unregister)
                return disable_binfmt();

        if (argc > optind) {
                int i;

                for (i = optind; i < argc; i++) {
                        k = apply_file(argv[i], false);
                        if (k < 0 && r == 0)
                                r = k;
                }
        } else {
                _cleanup_strv_free_ char **files = NULL;
                char **f;

                r = conf_files_list_strv(&files, ".conf", NULL, 0, (const char**) CONF_PATHS_STRV("binfmt.d"));
                if (r < 0)
                        return log_error_errno(r, "Failed to enumerate binfmt.d files: %m");

                if (arg_cat_config) {
                        pager_open(arg_pager_flags);

                        return cat_files(NULL, files, 0);
                }

                /* Flush out all rules */
                (void) write_string_file("/proc/sys/fs/binfmt_misc/status", "-1", WRITE_STRING_FILE_DISABLE_BUFFER);

                STRV_FOREACH(f, files) {
                        k = apply_file(*f, true);
                        if (k < 0 && r == 0)
                                r = k;
                }
        }

        return r;
}

DEFINE_MAIN_FUNCTION(run);
