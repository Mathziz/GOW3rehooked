#include <stdio.h>
#include "libavutil/file_open.h"

FILE *avpriv_fopen_utf8(const char *path, const char *mode)
{
    return fopen(path, mode);
}
