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

# Create a mininal Core Audio program
define create-coreaudio-prog
echo '\
#include <CoreAudio/CoreAudio.h>\n\
int main(){\n\
    AudioComponent comp;\n\
    comp = AudioComponentFindNext(NULL, &desc);\n\
    if (comp == NULL) exit (-1);\n\
    return 0;\n\
}\n'
endef

# Check ALSA installation
define check-alsa
$(shell $(call create-alsa-prog) | $(CC) -x c -lasound -o /dev/null > /dev/null 2> /dev/null - 
	&& echo $$?)
endef

# Check Core Audio installation
define check-coreaudio
$(shell $(call create-coreaudio-prog) | $(CC) -x c -framework CoreAudio -o /dev/null > /dev/null 2> /dev/null - 
	&& echo $$?) 
endef
