# Create a mininal ALSA program
define create-alsa-prog
echo '\
#include <alsa/asoundlib.h>\n\
int main(){\n\
    snd_pcm_t *pcm;\n\
    snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);\n\
    snd_pcm_close(pcm);\n\
    return 0;\n\
}\n'
endef

# Check ALSA installation
define check-alsa
$(shell $(call create-alsa-prog) | $(CC) -x c -lasound -o /dev/null > /dev/null 2> /dev/null - 
	&& echo $$?)
endef
