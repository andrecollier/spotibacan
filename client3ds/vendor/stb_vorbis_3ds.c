/* stb_vorbis plus the one hook the streaming decoder needs.
 *
 * Upstream's patched copy never shipped; set_length is the only symbol
 * native.c uses that stock stb_vorbis lacks. Seeding total_samples keeps
 * stb_vorbis_stream_length_in_samples (and seek) from scanning the tail of
 * a buffer that is still zero fill while the download runs. */
#include "stb_vorbis.c"

void stb_vorbis_set_length(stb_vorbis *f, unsigned int sample_number)
{
	f->total_samples = sample_number;
}
