#pragma once
// Keep cpp-httplib's inline transport definitions identical in every image.
#include "lr_win_poll.h"
#include <httplib.h>
#if defined(_WIN32)
#undef WSAPoll
#undef shutdown
#undef closesocket
#endif
