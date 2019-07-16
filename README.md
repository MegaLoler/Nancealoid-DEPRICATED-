# Nancealoid

Negative Nancy's Voice Synthesizer!

# how 2 make

type `make` duh

# how 2 use

u need jack audio server running

rn doesnt actually produce a sound source so u need to route one in (preferably a sawtooth-like wave if nothin better)

a visualizer would be cool eventually

need 2 add nasal cavities and "side of tongue" cavities n such

also need to make tract length vary slightly with pitch! since the larynx MOVES

and also probably have a "lip extrusion" parameter

and of course its own glottal sound source!

also...... trilling wld b v cool i want to figure out good way to simulate trills

also..... a way to interpolate between discrete tract lengths would b good... 

# midi parameters

use midi control signals to control various parameters

- 0x15 tongue frontness
- 0x16 tongue height
- 0x17 lips roundedness
- 0x18 tract length (higher = deeper)
- 0x19 "lethargy" (controls how sluggishly the tract reshapes itself)
- 0x1a continuous air pressure from lungs (can be positive or negative, basically its breathing out or in)
- 0x1b damping (how much sound energy gets absorbed during collisions; lower = louder resonant artifacts)

# midi channel 10

starting from note c2 (i think lol) and up, notes on midi channel 10 are mapped to some hardcoded phoneme presets
