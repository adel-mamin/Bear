/*  Copyright (C) 2012-2017 by László Nagy
    This file is part of Bear.

    Bear is a tool to generate compilation database for clang tooling.

    Bear is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Bear is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * This file implements a shared library. This library can be pre-loaded by
 * the dynamic linker of the Operating System (OS). It implements a few function
 * related to process creation. By pre-load this library the executed process
 * uses these functions instead of those from the standard library.
 *
 * The idea here is to inject a logic before call the real methods. The logic is
 * to dump the call into a file. To call the real method this library is doing
 * the job of the dynamic linker.
 *
 * The only input for the log writing is about the destination directory.
 * This is passed as environment variable.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

#if defined HAVE_XLOCALE_HEADER
#include <xlocale.h>
#endif

#if defined HAVE_POSIX_SPAWN || defined HAVE_POSIX_SPAWNP
#include <spawn.h>
#endif

#if defined HAVE_NSGETENVIRON
# include <crt_externs.h>
static char **environ;
#else
extern char **environ;
#endif

#define ENV_OUTPUT "INTERCEPT_BUILD_TARGET_DIR"
#ifdef APPLE
# define ENV_FLAT    "DYLD_FORCE_FLAT_NAMESPACE"
# define ENV_PRELOAD "DYLD_INSERT_LIBRARIES"
# define ENV_SIZE 3
#else
# define ENV_PRELOAD "LD_PRELOAD"
# define ENV_SIZE 2
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT "libear: (" __FILE__ ":" TOSTRING(__LINE__) ") "

#define PERROR(msg) do { perror(AT msg); } while (0)

#define ERROR_AND_EXIT(msg) do { PERROR(msg); exit(EXIT_FAILURE); } while (0)

#ifdef __CYGWIN__
#include <sys/cygwin.h>
void *RTLD_NEXT = NULL;
#endif

#define DLSYM(TYPE_, VAR_, SYMBOL_)                                 \
    union {                                                         \
        void *from;                                                 \
        TYPE_ to;                                                   \
    } cast;                                                         \
    if (!RTLD_NEXT) {    					    \
        RTLD_NEXT = dlopen("cygwin1.dll", 0);                       \
    }								    \
    if (0 == (cast.from = dlsym(RTLD_NEXT, SYMBOL_))) {             \
        PERROR("dlsym");                                            \
        exit(EXIT_FAILURE);                                         \
    }                                                               \
    TYPE_ const VAR_ = cast.to;


typedef char const * bear_env_t[ENV_SIZE];

static int capture_env_t(bear_env_t *env);
static void release_env_t(bear_env_t *env);
static char const **string_array_partial_update(char *const envp[], bear_env_t *env);
static char const **string_array_single_update(char const **in, char const *key, char const *value);
static void report_call(char const *const argv[]);
static void write_report(int fd, char const *const argv[]);
static int write_json_report(int fd, char const *const cmd[], char const *cwd, pid_t pid);
static int encode_json_string(char const *src, char *dst, size_t dst_size);
static char const **string_array_from_varargs(char const *arg, va_list *ap);
static char const **string_array_copy(char const **const in);
static size_t string_array_length(char const *const *in);
static void string_array_release(char const **);

#ifdef __CYGWIN__
__attribute__((constructor))
void _init(void) {
    cygwin_internal(CW_HOOK, "execl", execl);
    cygwin_internal(CW_HOOK, "execle", execle);
    cygwin_internal(CW_HOOK, "execlp", execlp);
    cygwin_internal(CW_HOOK, "execv", execv);
    cygwin_internal(CW_HOOK, "execve", execve);
    cygwin_internal(CW_HOOK, "execvp", execvp);
    cygwin_internal(CW_HOOK, "execvpe", execvpe);
    cygwin_internal(CW_HOOK, "posix_spawn", posix_spawn);
    cygwin_internal(CW_HOOK, "posix_spawnp", posix_spawnp);
}
#endif


static bear_env_t env_names =
    { ENV_OUTPUT
    , ENV_PRELOAD
#ifdef ENV_FLAT
    , ENV_FLAT
#endif
    };

static bear_env_t initial_env =
    { 0
    , 0
#ifdef ENV_FLAT
    , 0
#endif
    };

static int initialized = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static locale_t utf_locale;

static void on_load(void) __attribute__((constructor));
static void on_unload(void) __attribute__((destructor));

static int mt_safe_on_load(void);
static void mt_safe_on_unload(void);


#ifdef HAVE_EXECVE
static int call_execve(const char *path, char *const argv[],
                       char *const envp[]);
#endif
#ifdef HAVE_EXECVP
static int call_execvp(const char *file, char *const argv[]);
#endif
#ifdef HAVE_EXECVPE
static int call_execvpe(const char *file, char *const argv[],
                        char *const envp[]);
#endif
#ifdef HAVE_EXECVP2
static int call_execvP(const char *file, const char *search_path,
                       char *const argv[]);
#endif
#ifdef HAVE_EXECT
static int call_exect(const char *path, char *const argv[],
                      char *const envp[]);
#endif
#ifdef HAVE_POSIX_SPAWN
static int call_posix_spawn(pid_t *restrict pid, const char *restrict path,
                            const posix_spawn_file_actions_t *file_actions,
                            const posix_spawnattr_t *restrict attrp,
                            char *const argv[restrict],
                            char *const envp[restrict]);
#endif
#ifdef HAVE_POSIX_SPAWNP
static int call_posix_spawnp(pid_t *restrict pid, const char *restrict file,
                             const posix_spawn_file_actions_t *file_actions,
                             const posix_spawnattr_t *restrict attrp,
                             char *const argv[restrict],
                             char *const envp[restrict]);
#endif


/* Initialization method to Captures the relevant environment variables.
 */

static void on_load(void) {
    pthread_mutex_lock(&mutex);
    if ((!initialized) && (mt_safe_on_load()))
        initialized = 1;
    pthread_mutex_unlock(&mutex);
}

static void on_unload(void) {
    pthread_mutex_lock(&mutex);
    if (initialized)
        mt_safe_on_unload();
    initialized = 0;
    pthread_mutex_unlock(&mutex);
}

static int mt_safe_on_load(void) {
#ifdef HAVE_NSGETENVIRON
    environ = *_NSGetEnviron();
    if (0 == environ)
        return 0;
#endif
    // Create locale to encode UTF-8 characters
    utf_locale = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
    if ((locale_t)0 == utf_locale) {
        PERROR("newlocale");
        return 0;
    }
    // Capture current relevant environment variables
    if (0 == capture_env_t(&initial_env))
        return 0;
    // Well done
    return 1;
}

static void mt_safe_on_unload(void) {
    freelocale(utf_locale);
    release_env_t(&initial_env);
}


/* These are the methods we are try to hijack.
 */

#ifdef HAVE_EXECVE
int execve(const char *path, char *const argv[], char *const envp[]) {
    report_call((char const *const *)argv);
    return call_execve(path, argv, envp);
}
#endif

#ifdef HAVE_EXECV
#ifndef HAVE_EXECVE
#error can not implement execv without execve
#endif
int execv(const char *path, char *const argv[]) {
    report_call((char const *const *)argv);
    return call_execve(path, argv, environ);
}
#endif

#ifdef HAVE_EXECVPE
int execvpe(const char *file, char *const argv[], char *const envp[]) {
    report_call((char const *const *)argv);
    return call_execvpe(file, argv, envp);
}
#endif

#ifdef HAVE_EXECVP
int execvp(const char *file, char *const argv[]) {
    report_call((char const *const *)argv);
    return call_execvp(file, argv);
}
#endif

#ifdef HAVE_EXECVP2
int execvP(const char *file, const char *search_path, char *const argv[]) {
    report_call((char const *const *)argv);
    return call_execvP(file, search_path, argv);
}
#endif

#ifdef HAVE_EXECT
int exect(const char *path, char *const argv[], char *const envp[]) {
    report_call((char const *const *)argv);
    return call_exect(path, argv, envp);
}
#endif

#ifdef HAVE_EXECL
# ifndef HAVE_EXECVE
#  error can not implement execl without execve
# endif
int execl(const char *path, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char const **argv = string_array_from_varargs(arg, &args);
    va_end(args);

    report_call((char const *const *)argv);
    int const result = call_execve(path, (char *const *)argv, environ);

    string_array_release(argv);
    return result;
}
#endif

#ifdef HAVE_EXECLP
# ifndef HAVE_EXECVP
#  error can not implement execlp without execvp
# endif
int execlp(const char *file, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char const **argv = string_array_from_varargs(arg, &args);
    va_end(args);

    report_call((char const *const *)argv);
    int const result = call_execvp(file, (char *const *)argv);

    string_array_release(argv);
    return result;
}
#endif

#ifdef HAVE_EXECLE
# ifndef HAVE_EXECVE
#  error can not implement execle without execve
# endif
// int execle(const char *path, const char *arg, ..., char * const envp[]);
int execle(const char *path, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char const **argv = string_array_from_varargs(arg, &args);
    char const **envp = va_arg(args, char const **);
    va_end(args);

    report_call((char const *const *)argv);
    int const result =
        call_execve(path, (char *const *)argv, (char *const *)envp);

    string_array_release(argv);
    return result;
}
#endif

#ifdef HAVE_POSIX_SPAWN
int posix_spawn(pid_t *restrict pid, const char *restrict path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *restrict attrp,
                char *const argv[restrict], char *const envp[restrict]) {
    report_call((char const *const *)argv);
    return call_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}
#endif

#ifdef HAVE_POSIX_SPAWNP
int posix_spawnp(pid_t *restrict pid, const char *restrict file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *restrict attrp,
                 char *const argv[restrict], char *const envp[restrict]) {
    report_call((char const *const *)argv);
    return call_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
}
#endif

/* These are the methods which forward the call to the standard implementation.
 */

#ifdef HAVE_EXECVE
static int call_execve(const char *path, char *const argv[],
                       char *const envp[]) {
    typedef int (*func)(const char *, char *const *, char *const *);

    DLSYM(func, fp, "execve");

    char const **const menvp = string_array_partial_update(envp, &initial_env);
    int const result = (*fp)(path, argv, (char *const *)menvp);
    string_array_release(menvp);
    return result;
}
#endif

#ifdef HAVE_EXECVPE
static int call_execvpe(const char *file, char *const argv[],
                        char *const envp[]) {
    typedef int (*func)(const char *, char *const *, char *const *);

    DLSYM(func, fp, "execvpe");

    char const **const menvp = string_array_partial_update(envp, &initial_env);
    int const result = (*fp)(file, argv, (char *const *)menvp);
    string_array_release(menvp);
    return result;
}
#endif

#ifdef HAVE_EXECVP
static int call_execvp(const char *file, char *const argv[]) {
    typedef int (*func)(const char *file, char *const argv[]);

    DLSYM(func, fp, "execvp");

    char **const original = environ;
    char const **const modified = string_array_partial_update(original, &initial_env);
    environ = (char **)modified;
    int const result = (*fp)(file, argv);
    environ = original;
    string_array_release(modified);

    return result;
}
#endif

#ifdef HAVE_EXECVP2
static int call_execvP(const char *file, const char *search_path,
                       char *const argv[]) {
    typedef int (*func)(const char *, const char *, char *const *);

    DLSYM(func, fp, "execvP");

    char **const original = environ;
    char const **const modified = string_array_partial_update(original, &initial_env);
    environ = (char **)modified;
    int const result = (*fp)(file, search_path, argv);
    environ = original;
    string_array_release(modified);

    return result;
}
#endif

#ifdef HAVE_EXECT
static int call_exect(const char *path, char *const argv[],
                      char *const envp[]) {
    typedef int (*func)(const char *, char *const *, char *const *);

    DLSYM(func, fp, "exect");

    char const **const menvp = string_array_partial_update(envp, &initial_env);
    int const result = (*fp)(path, argv, (char *const *)menvp);
    string_array_release(menvp);
    return result;
}
#endif

#ifdef HAVE_POSIX_SPAWN
static int call_posix_spawn(pid_t *restrict pid, const char *restrict path,
                            const posix_spawn_file_actions_t *file_actions,
                            const posix_spawnattr_t *restrict attrp,
                            char *const argv[restrict],
                            char *const envp[restrict]) {
    typedef int (*func)(pid_t *restrict, const char *restrict,
                        const posix_spawn_file_actions_t *,
                        const posix_spawnattr_t *restrict,
                        char *const *restrict, char *const *restrict);

    DLSYM(func, fp, "posix_spawn");

    char const **const menvp = string_array_partial_update(envp, &initial_env);
    int const result =
        (*fp)(pid, path, file_actions, attrp, argv, (char *const *restrict)menvp);
    string_array_release(menvp);
    return result;
}
#endif

#ifdef HAVE_POSIX_SPAWNP
static int call_posix_spawnp(pid_t *restrict pid, const char *restrict file,
                             const posix_spawn_file_actions_t *file_actions,
                             const posix_spawnattr_t *restrict attrp,
                             char *const argv[restrict],
                             char *const envp[restrict]) {
    typedef int (*func)(pid_t *restrict, const char *restrict,
                        const posix_spawn_file_actions_t *,
                        const posix_spawnattr_t *restrict,
                        char *const *restrict, char *const *restrict);

    DLSYM(func, fp, "posix_spawnp");

    char const **const menvp = string_array_partial_update(envp, &initial_env);
    int const result =
        (*fp)(pid, file, file_actions, attrp, argv, (char *const *restrict)menvp);
    string_array_release(menvp);
    return result;
}
#endif

/* this method is to write log about the process creation. */

static void report_call(char const *const argv[]) {
    if (!initialized)
        return;
    // Create report file name
    char const * const out_dir = initial_env[0];
    size_t const path_max_length = strlen(out_dir) + 32;
    char filename[path_max_length];
    if (-1 == snprintf(filename, path_max_length, "%s/execution.XXXXXX", out_dir))
        ERROR_AND_EXIT("snprintf");
    // Create report file
    int fd = mkstemp((char *)&filename);
    if (-1 == fd)
        ERROR_AND_EXIT("mkstemp");
    // Write report file
    write_report(fd, argv);
    // Close report file
    if (close(fd))
        ERROR_AND_EXIT("close");
}

static void write_report(int fd, char const *const argv[]) {
    const locale_t saved_locale = uselocale(utf_locale);
    if ((locale_t)0 == saved_locale)
        ERROR_AND_EXIT("uselocale");

    const char *cwd = getcwd(NULL, 0);
    if (0 == cwd)
        ERROR_AND_EXIT("getcwd");
    if (write_json_report(fd, argv, cwd, getpid()))
        ERROR_AND_EXIT("writing json problem");
    free((void *)cwd);

    const locale_t restored_locale = uselocale(saved_locale);
    if ((locale_t)0 == restored_locale)
        ERROR_AND_EXIT("uselocale");
}

static int write_json_report(int fd, char const *const cmd[], char const *const cwd, pid_t pid) {
    if (0 > dprintf(fd, "{ \"pid\": %d, \"cmd\": [", pid))
        return -1;

    for (char const *const *it = cmd; (it) && (*it); ++it) {
        char const *const sep = (it != cmd) ? "," : "";
        const size_t buffer_size = (6 * strlen(*it)) + 1;
        char buffer[buffer_size];
        if (-1 == encode_json_string(*it, buffer, buffer_size))
            return -1;
        if (0 > dprintf(fd, "%s \"%s\"", sep, buffer))
            return -1;
    }
    const size_t buffer_size = 6 * strlen(cwd);
    char buffer[buffer_size];
    if (-1 == encode_json_string(cwd, buffer, buffer_size))
        return -1;
    if (0 > dprintf(fd, "], \"cwd\": \"%s\" }", buffer))
        return -1;

    return 0;
}

static int encode_json_string(char const *const src, char *const dst, size_t const dst_size) {
    size_t const wsrc_length = mbstowcs(NULL, src, 0);
    wchar_t wsrc[wsrc_length + 1];
    if (mbstowcs((wchar_t *)&wsrc, src, wsrc_length + 1) != wsrc_length) {
        PERROR("mbstowcs");
        return -1;
    }
    wchar_t const *wsrc_it = (wchar_t const *)&wsrc;
    wchar_t const *const wsrc_end = wsrc_it + wsrc_length;

    char *dst_it = dst;
    char *const dst_end = dst + dst_size;

    for (; wsrc_it != wsrc_end; ++wsrc_it) {
        if (dst_it >= dst_end) {
            return -1;
        }
        // Insert an escape character before control characters.
        switch (*wsrc_it) {
        case L'\b':
            dst_it += snprintf(dst_it, 3, "\\b");
            break;
        case L'\f':
            dst_it += snprintf(dst_it, 3, "\\f");
            break;
        case L'\n':
            dst_it += snprintf(dst_it, 3, "\\n");
            break;
        case L'\r':
            dst_it += snprintf(dst_it, 3, "\\r");
            break;
        case L'\t':
            dst_it += snprintf(dst_it, 3, "\\t");
            break;
        case L'"':
            dst_it += snprintf(dst_it, 3, "\\\"");
            break;
        case L'\\':
            dst_it += snprintf(dst_it, 3, "\\\\");
            break;
        default:
            if ((*wsrc_it < L' ') || (*wsrc_it > 127)) {
                dst_it += snprintf(dst_it, 7, "\\u%04x", (unsigned int)*wsrc_it);
            } else {
                *dst_it++ = (char)*wsrc_it;
            }
            break;
        }
    }
    if (dst_it < dst_end) {
        // Insert a terminating 0 value.
        *dst_it = 0;
        return 0;
    }
    return -1;
}

/* update environment assure that chilren processes will copy the desired
 * behaviour */

static int capture_env_t(bear_env_t *env) {
    int status = 1;
    for (size_t it = 0; it < ENV_SIZE; ++it) {
        char const * const env_value = getenv(env_names[it]);
        char const * const env_copy = (env_value) ? strdup(env_value) : env_value;
        (*env)[it] = env_copy;
        status &= (env_copy) ? 1 : 0;
        // Just report the problem, but don't roll back.
        if (0 == status)
            PERROR("strdup");
    }
    return status;
}

static void release_env_t(bear_env_t *env) {
    for (size_t it = 0; it < ENV_SIZE; ++it) {
        free((void *)(*env)[it]);
        (*env)[it] = 0;
    }
}

static char const **string_array_partial_update(char *const envp[], bear_env_t *env) {
    char const **result = string_array_copy((char const **)envp);
    for (size_t it = 0; it < ENV_SIZE && (*env)[it]; ++it)
        result = string_array_single_update(result, env_names[it], (*env)[it]);
    return result;
}

static char const **string_array_single_update(char const *envs[], char const *key, char const * const value) {
    // find the key if it's there
    size_t const key_length = strlen(key);
    char const **it = envs;
    for (; (it) && (*it); ++it) {
        if ((0 == strncmp(*it, key, key_length)) &&
            (strlen(*it) > key_length) && ('=' == (*it)[key_length]))
            break;
    }
    // allocate a environment entry
    size_t const value_length = strlen(value);
    size_t const env_length = key_length + value_length + 2;
    char *env = malloc(env_length);
    if (0 == env)
        ERROR_AND_EXIT("malloc");
    if (-1 == snprintf(env, env_length, "%s=%s", key, value))
        ERROR_AND_EXIT("snprintf");
    // replace or append the environment entry
    if (it && *it) {
        free((void *)*it);
        *it = env;
	    return envs;
    } else {
        size_t const size = string_array_length(envs);
        char const **result = realloc(envs, (size + 2) * sizeof(char const *));
        if (0 == result)
            ERROR_AND_EXIT("realloc");
        result[size] = env;
        result[size + 1] = 0;
        return result;
    }
}

/* util methods to deal with string arrays. environment and process arguments
 * are both represented as string arrays. */

static char const **string_array_from_varargs(char const *const arg, va_list *args) {
    char const **result = 0;
    size_t size = 0;
    for (char const *it = arg; it; it = va_arg(*args, char const *)) {
        result = realloc(result, (size + 1) * sizeof(char const *));
        if (0 == result)
            ERROR_AND_EXIT("realloc");
        char const *copy = strdup(it);
        if (0 == copy)
            ERROR_AND_EXIT("strdup");
        result[size++] = copy;
    }
    result = realloc(result, (size + 1) * sizeof(char const *));
    if (0 == result)
        ERROR_AND_EXIT("realloc");
    result[size++] = 0;

    return result;
}

static char const **string_array_copy(char const **const in) {
    size_t const size = string_array_length(in);

    char const **const result = malloc((size + 1) * sizeof(char const *));
    if (0 == result)
        ERROR_AND_EXIT("malloc");

    char const **out_it = result;
    for (char const *const *in_it = in; (in_it) && (*in_it);
         ++in_it, ++out_it) {
        *out_it = strdup(*in_it);
        if (0 == *out_it)
            ERROR_AND_EXIT("strdup");
    }
    *out_it = 0;
    return result;
}

static size_t string_array_length(char const *const *const in) {
    size_t result = 0;
    for (char const *const *it = in; (it) && (*it); ++it)
        ++result;
    return result;
}

static void string_array_release(char const **in) {
    for (char const *const *it = in; (it) && (*it); ++it) {
        free((void *)*it);
    }
    free((void *)in);
}
