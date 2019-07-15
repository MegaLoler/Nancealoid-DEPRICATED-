# Nancealoid

Negative Nancy's Voice Synthesizer!

# how 2 make

type `make` duh

# how 2 use

u need jack audio server running

the `tract` program simulates the resonant filtering of the vocal tract

the `glottis` program simulates a glottal pulse train

u might want to route the audio like this:
glottis -> tract -> system out


gonna make `glottis` accept midi input for playing notes

and probably make `tract` accept midi control signals for changing the shape of the tract

eventually it'd b cool to assign midi notes on channel 10 to preset vocal tract shapes corresponding to different phonemes
