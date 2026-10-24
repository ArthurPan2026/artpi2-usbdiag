#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

int web_server_start(void);
int web_server_is_online(void);

/* Render one route into a caller-owned buffer.  The Mongoose event loop calls
 * this synchronously, then copies the result into its connection iobuf. */
int web_server_render(char *payload, rt_size_t payload_size,
                      const char *path, const char **content_type,
                      rt_bool_t accepts_gzip, rt_bool_t *gzip_encoded);

#ifdef __cplusplus
}
#endif

#endif
