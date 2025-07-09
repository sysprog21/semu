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

# Create a mininal PulseAudio program
define create-pa-prog
echo '\
#include <pulse/mainloop.h>\n\
int main(){\n\
    pa_mainloop *m;\n\
    return 0;\n\
}\n'
endef

# Create a mininal CoreAudio program
define create-ca-prog
echo '\
#include <CoreAudio/CoreAudio.h>\n\
#include <AudioToolbox/AudioQueue.h>\n\
int main(){\n\
    AudioQueueRef queue;\n\
    AudioQueueDispose(queue, TRUE);\n\
    return 0;\n\
}\n'
endef

# Create a mininal jack2 program
define create-jack2-prog
echo '\
#include <stdlib.h>\n\
#include <jack/jack.h>\n\
int main(){\n\
	jack_default_audio_sample_t *signal;\n\
	signal = (jack_default_audio_sample_t *)malloc(1024 * sizeof(jack_default_audio_sample_t));\n\
	free(signal);\n\
	return 0;\n\
}\n'
endef

# Check ALSA installation
define check-alsa
$(shell $(call create-alsa-prog) | $(CC) -x c - -lasound -o /dev/null > /dev/null 2> /dev/null 
	&& echo 0 || echo 1)
endef

# Check PulseAudio installation
define check-pa
$(shell $(call create-pa-prog) | $(CC) -x c - -lpulse -o /dev/null > /dev/null 2> /dev/null && echo 0 || echo 1)
endef

# Check CoreAudio installation
define check-coreaudio
$(shell $(call create-ca-prog) | $(CC) -x c - -framework AudioToolbox -o /dev/null > /dev/null 2> /dev/null 
	&& echo 0 || echo 1)
endef

# Check JACK (formally jack2) installation
define check-jack2
$(shell $(call create-jack2-prog) | $(CC) -x c - -ljack  -o /dev/null > /dev/null 2> /dev/null 
	&& echo 0 || echo 1)
endef
