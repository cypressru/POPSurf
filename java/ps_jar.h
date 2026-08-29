/* Reading a .jar.
 *
 * A jar is a zip, and the two entry compressions it uses are "stored" and
 * "deflate" - nothing else has ever appeared in one produced by a Java
 * toolchain. Deflate comes from stb_image's inflate, which is already linked
 * for PNG and is the raw, headerless variety a zip entry carries.
 *
 * Only the central directory is walked. Scanning for local file headers is the
 * usual shortcut and it is wrong on any archive that has been appended to or
 * has entries whose local header omits the sizes - both of which real jars do,
 * because the sizes are only known after compressing.
 *
 * This reads; it does not verify. A jar off the open web is untrusted input,
 * so every offset is bounds-checked against the buffer and every declared size
 * against a ceiling before anything is allocated.
 */
#ifndef PS_JAR_H
#define PS_JAR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per entry whose name ends in ".class".
 *
 * `name` is the entry path with the suffix stripped and separators left as
 * they are, which is already the internal form a class file refers to itself
 * by - "com/example/Sprite". `data` is freshly allocated and handed over.
 * Return non-zero to keep going, zero to stop early. */
typedef int (*ps_jar_entry_fn)(void *user, const char *name, uint8_t *data,
                              size_t len);

/* Walks the archive. Returns the number of class entries delivered, or -1 if
 * the file is not a readable zip. */
int ps_jar_read(const uint8_t *buf, size_t len, ps_jar_entry_fn fn,
                void *user);

#ifdef __cplusplus
}
#endif

#endif /* PS_JAR_H */
