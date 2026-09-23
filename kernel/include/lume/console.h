/*
 * The console character device (kernel interface).
 *
 * See kernel/drivers/console.c for what it does and does not implement.
 */
#ifndef LUME_CONSOLE_H
#define LUME_CONSOLE_H

#include <lume/fs.h>

/** The console device node.  Static storage: it exists for the whole boot. */
struct fs_node *console_device(void);

#endif /* LUME_CONSOLE_H */
