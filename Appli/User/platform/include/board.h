#ifndef WORK_APPLI_BOARD_COMPAT_H
#define WORK_APPLI_BOARD_COMPAT_H

/* The official ART-Pi2 pin driver expects board.h.  Reuse only its board
 * constants while keeping this CMake project independent of the full BSP. */
#include "../../../ThirdParty/artpi2/board/board.h"
#include "../../../ThirdParty/rt-thread/components/drivers/include/drivers/dev_pin.h"

#endif
