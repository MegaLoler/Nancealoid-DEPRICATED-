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

also a visualizer wld b cool







probably gonna rearchitecture this whole thing!!
merge them into one program


need to figure out how to simulation plosion and frication
also need to add nasal cavity
also need to make tract length vary slightly with pitch! since the larynx MOVES
