/* stb_image, for the host builds that need it and have no ps_image.o.
 *
 * ps_jar's inflate is stb_image's, and ps_applet decodes GIF and PNG with the
 * rest of it, so any host runner linking those two needs one translation unit
 * carrying the implementation. jrun.c has one inline; japplet.c did not.
 * Splitting it out means both runners share the same one. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"
