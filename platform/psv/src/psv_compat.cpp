#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cwctype>
#include <regex.h>
#include <sys/reent.h>
#include <sys/types.h>

extern "C" {

int sigprocmask(int, const sigset_t*, sigset_t*)
{
    return 0;
}

int regcomp(regex_t*, const char*, int)
{
    return REG_ESPACE;
}

int regexec(const regex_t*, const char*, std::size_t, regmatch_t[], int)
{
    return REG_NOMATCH;
}

void regfree(regex_t*) {}

pid_t _wait_r(struct _reent* reent, int*)
{
    if (reent)
        reent->_errno = ECHILD;
    errno = ECHILD;
    return -1;
}

wint_t _jp2uc_l(wint_t value, void*)
{
    return value;
}

wint_t _uc2jp_l(wint_t value, void*)
{
    return value;
}

}
