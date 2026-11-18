/* ETC1 / ETC2 texture encoder: pictures are compressed on the device after decoding, so the GPU keeps (and the disk
   cache stores) 4 to 8 times fewer bytes than RGBA. Blocks are row-major, 4x4 pixels each; the size of the picture
   need not be a multiple of 4 (edge blocks repeat the last row and column). */
#ifndef SB_ETC_H
#define SB_ETC_H
#include <stddef.h>

size_t etc_size(int w, int h, int alpha); /* bytes of the blocks: 8 per block (RGB), 16 with alpha */
/* GL_COMPRESSED_RGB8_ETC2 (and GL_ETC1_RGB8_OES: the same bits); alpha is ignored */
void etc1_encode(const unsigned char *rgba, int w, int h, unsigned char *out);
/* GL_COMPRESSED_RGBA8_ETC2_EAC: 8 bytes of alpha, then 8 of colour, per block */
void etc2_eac_encode(const unsigned char *rgba, int w, int h, unsigned char *out);
#endif
