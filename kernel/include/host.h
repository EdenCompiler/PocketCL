#ifndef POCKET_HOST_H
#define POCKET_HOST_H

#include "pocket.h"

/* Small stdio boundary used by stage-0 primitives. Callers measure/reserve
 * arena space before allocating a result, then pass the preallocated result
 * to the read routine. These helpers never collect. */
bool p_host_file_size(Pocket *p, Value path, size_t *size);
bool p_host_read_file(Pocket *p, Value path, Value bytes);
bool p_host_write_file(Pocket *p, Value path, Value bytes);
bool p_host_console_write_string(Pocket *p, Value string);
bool p_host_console_read_line(Pocket *p, char *out, size_t capacity,
                              size_t *written, bool *eof);

#endif
