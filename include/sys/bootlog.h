#ifndef SYS_BOOTLOG_H
#define SYS_BOOTLOG_H

void bootlog_stage(const char *msg);
const char *bootlog_buffer(void);
int bootlog_buffer_size(void);

#endif
