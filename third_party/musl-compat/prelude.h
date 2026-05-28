/*
 * Force-included compatibility prelude.
 *
 * libvfio-user.h uses loff_t but only includes <sys/uio.h>. glibc pulls loff_t
 * in transitively; musl does not -- it defines `loff_t` (as off_t) in
 * <fcntl.h>. Including it here keeps libvfio-user.h compiling on musl, and is a
 * no-op on glibc.
 *
 * musl is also stricter about transitive includes: libvfio-user relies on
 * <string.h>/<stdlib.h> being pulled in indirectly (as glibc does), so include
 * them here too.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
