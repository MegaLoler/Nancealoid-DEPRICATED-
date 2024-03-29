/*
 * nancealoid
 *
 * this program simulates a vocal tract using a 1d digital waveguide
 * produces a glottal pulse train that is filtered by the tract
 * outputs the result
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <jack/jack.h>
#include <jack/midiport.h>

#define SPEED_OF_SOUND 34300    // cm per second
#define TRACT_LENGTH 17.5       // desired tract length in cm
#define NEUTRAL_Z 1             // impedence of schwa
#define THROAT_Z 5              // impedence of throat
#define DRAIN_Z 0.1             // acoustic impedence at the opening of the lips
#define MIN_AREA 0.000001       // to avoid divisions by 0 lol

// midi controllers for different functions
#define CONTROLLER_TONGUE_POSITION 0x15
#define CONTROLLER_TONGUE_HEIGHT 0x16
#define CONTROLLER_LIPS_ROUNDEDNESS 0x17
#define CONTROLLER_TRACT_LENGTH 0x18
#define CONTROLLER_DRAG 0x19
#define CONTROLLER_PRESSURE 0x1a
#define CONTROLLER_DAMPING 0x1b

// controller ranges
#define CONTROLLER_TRACT_LENGTH_MIN 8
#define CONTROLLER_TRACT_LENGTH_MAX 24

// tongue start and stop (percentage of tract)
#define TONGUE_BACK 0.2
#define TONGUE_FRONT 0.9

// how fast to sitch between phonemes
#define DEFAULT_INTERPOLATION_DRAG 0.0004
#define DRAG_MIN 0.001
#define DRAG_MAX 0.0001

double interpolation_drag;

// min and max continuous air pressure from the lungs
#define MIN_DIAPHRAM_PRESSURE -0.2
#define MAX_DIAPHRAM_PRESSURE 0.2

double diaphram_pressure;

// how much acoustic energy is absorbed in collisions
#define DEFAULT_DAMPING 0.04
#define MIN_DAMPING 0
#define MAX_DAMPING 0.2

double damping;

// frication multiplier
#define FRICATION 0.1

// TODO: make this actuall work lol NEED ME SOME TRILLS
// so like, sound pressure can actually reshape the tract
// and it will oscillate
// this is the damping factor
#define PHYSICAL_DAMPING 1
// and how rigid various parts are
#define LIPS_RIGIDITY 1

// which midi channel to use to map notes to phonemes
#define PHONEME_CHANNEL 0x9

//#define DEBUG_TRACT

jack_port_t *midi_input_port;
jack_port_t *input_port;
jack_port_t *output_port;
jack_client_t *client;

// a waveguide segment
struct Segment {
    double z; // acoustic impedence at this segment (inverse of cross sectional area (i think lol))
    double target_z; // where it wants to be
    double rigidity; // 1 = will not move at all
    jack_default_audio_sample_t left; // acoustic energy traveling left
    jack_default_audio_sample_t right; // acoustic energy traveling left
};

// vocal tract stuff
int rate; // sample rate
double unit_length; // length of segment in cm
double tract_length; // length of tract in cm
int nsegments; // number of segments

// "double buffer" the waveguide segments lol
struct Segment *segments_front; // front buffer
struct Segment *segments_back; // back buffer
struct Segment *buffer1;
struct Segment *buffer2;

// represents a shape of the mouth to produce a certain sound
struct Phoneme {
    // tract shape stuff
    // vowel space
    double tongue_height; // closedness
    double tongue_position; // backness
    double lips_roundedness;
};

// SOME PHONEMES
struct Phoneme PHONEME_A = { 0.9, 0, 0 };
struct Phoneme PHONEME_I = { 0.9, 1, 0 };
struct Phoneme PHONEME_U = { 0, 0, 0.9 };
struct Phoneme PHONEME_E = { 0.9, 0.5, 0 };
struct Phoneme PHONEME_O = { 0.9, 0.25, 0.9 };
struct Phoneme PHONEME_SCHWA = { 0, 0, 0 };
struct Phoneme PHONEME_UH = { 0.7, 0, 0.6 };
struct Phoneme PHONEME_AH = { 0.7, 0, 0 };
struct Phoneme PHONEME_UE = { 0.9, 1, 0.9 };
struct Phoneme PHONEME_II = { 0.9, 0.75, 0 };
struct Phoneme PHONEME_OE = { 0, 0, 0.75 };

// phoneme to return to
// controlled freely by midi control signals
struct Phoneme ambient_phoneme;

// target phoneme
// point it to what you want the phoneme to be
// simulation will interpolate towards it
struct Phoneme *target_phoneme;

// represents the ACTUAL CURRENT INSTANT shape of the mouth
struct Phoneme current_phoneme;

// return a pointer to a phoneme that is mapped to a midi note value
struct Phoneme *get_mapped_phoneme(uint8_t note) {
    // TODO: better means of mapping lol
    switch(note){
        case 0x24: return &PHONEME_A;
        case 0x25: return &PHONEME_I;
        case 0x26: return &PHONEME_U;
        case 0x27: return &PHONEME_E;
        case 0x28: return &PHONEME_O;
        case 0x29: return &PHONEME_SCHWA;
        case 0x2a: return &PHONEME_UH;
        case 0x2b: return &PHONEME_AH;
        case 0x2c: return &PHONEME_UE;
        case 0x2d: return &PHONEME_II;
        case 0x2e: return &PHONEME_OE;
        default: return &ambient_phoneme;
    }
}

// swap buffers by swapping pointers
void swap_buffers() {
    if(segments_front == buffer1) {
        segments_front = buffer2;
        segments_back = buffer1;
    } else {
        segments_front = buffer1;
        segments_back = buffer2;
    }
}

// update the shape of the tract
// using tongue height and position
// to approximate vowel sounds in "vowel space"
void update_shape(int set_z) {
    // approximate shape using cosine
    // position = 0 is all the way back
    // and 1 = all the way up front
    
    // get the start and stopping segments
    int start = TONGUE_BACK * nsegments;
    int stop = TONGUE_FRONT * nsegments;
    int ntongue = stop - start;

    // iterate over all the segments
    for(int i = 0; i < nsegments; i++) {
        struct Segment *s = &(segments_front[i]);
        
        if(i < start) {
            // throat
            s->target_z = THROAT_Z;
        } else if (i >= stop) {
            // front of mouth
            s->target_z = 1 / (1 - current_phoneme.lips_roundedness + MIN_AREA) * NEUTRAL_Z;
            s->rigidity = LIPS_RIGIDITY;
        } else {
            // tongue
            double unit_pos = (i - start) / (double)(ntongue - 1);
            double phase = unit_pos - current_phoneme.tongue_position;
            double value = cos(phase * M_PI / 2) * current_phoneme.tongue_height;
            double unit_area = 1 - value;
            s->target_z = 1 / (unit_area + MIN_AREA) * NEUTRAL_Z;
        }
        if(set_z)
            s->z = s->target_z;
    }
}

// initialize the vocal tract given a desired length in cm
void init_tract(double desired_length) {

    // get sampling rate
    rate = jack_get_sample_rate(client);

    // get length of a single segment of the waveguide in cm
    unit_length = (double)SPEED_OF_SOUND / rate;

    // get a number of segments that approximates the desired length
    nsegments = (int)(desired_length / unit_length);

    // the actual length
    tract_length = nsegments * unit_length;

    // allocate memory for the segments of the waveguide
    buffer1 = malloc(sizeof(struct Segment) * nsegments);
    buffer2 = malloc(sizeof(struct Segment) * nsegments);

    // setup the front and back buffer pointers
    segments_front = buffer1;
    segments_back = buffer2;

    // initialize the segments
    for(int i = 0; i < nsegments; i++) {
        // segments for the front and back buffers
        struct Segment *f = &(segments_front[i]);
        struct Segment *b = &(segments_back[i]);
        // init front buffer
        f->z = NEUTRAL_Z;
        f->target_z = NEUTRAL_Z;
        f->rigidity = 1;
        f->left = 0;
        f->right = 0;
        // init back buffer
        b->z = NEUTRAL_Z;
        b->target_z = NEUTRAL_Z;
        b->rigidity = 1;
        b->left = 0;
        b->right = 0;
    }

#ifdef DEBUG_TRACT
    // test impulse
    segments_front->right = 1;
    ambient_phoneme.lips_roundedness = 1;
    current_phoneme.lips_roundedness = 1;
#endif

    // test set the tract shape
    //segments_front[nsegments-2].z = 10/NEUTRAL_Z;

    // init the tract shape
    update_shape(1);

    // print some INTERESTING INFORMATION,
    printf("rate = %ihz\n", rate);
    printf("desired tract length = %fcm\n", desired_length);
    printf("actual tract length = %fcm\n", tract_length);
    printf("unit length = %fcm\n", unit_length);
    printf("num waveguide segments = %i\n", nsegments);
}

void resize_tract(double desired_length) {
    int old_nsegments = nsegments;
    struct Segment *old1 = buffer1;
    struct Segment *old2 = buffer2;

    // create a new tract of desired length
    // then copy over old values to avoid artifacts
    init_tract(desired_length);
    int n = old_nsegments < nsegments ? old_nsegments : nsegments;
    for(int i = 0; i < n; i++) {
        buffer1[i].left = old1[i].left;
        buffer1[i].right = old1[i].right;
        buffer2[i].left = old2[i].left;
        buffer2[i].right = old2[i].right;
    }

    // and free the old tract
    free(old1);
    free(old2);
}

void free_tract() {
    free(buffer1);
    free(buffer2);
    segments_front = NULL;
    segments_back = NULL;
}

void debug_tract(struct Segment *front, struct Segment *back) {
    for(int i = 0; i < nsegments; i++) {
        struct Segment f = front[i];
        struct Segment b = back[i];
        printf("SEG#%02d:\tZ=%2.2f\tTZ=%2.2f\tR=%2.2f\t\tL=%2.2f\tR=%2.2f\t\tL=%2.2f\tR=%2.2f\n", i, f.z, f.target_z, f.rigidity, f.left, f.right, b.left, b.right);
    }
}

// calculate a reflection coefficient
// given source impedence and target impedence
double reflection(double source_z, double target_z) {
    return (target_z - source_z) / (target_z + source_z);
}

// generate noise
double noise() {
    return rand() / (RAND_MAX / 2.0) - 1;
}

// run the vocal tract for the length of a single sample
// given the sample for the glottal source
// return the tract out
jack_default_audio_sample_t run_tract(jack_default_audio_sample_t glottal_source) {

    // front buffer is the "old" buffer
    // back buffer is where the changes get written
    // after calculating the changes the buffers are swapped
    // so the changes are "put into effect"

    // sound exiting the mouth
    jack_default_audio_sample_t drain = 0;

    // initialize the new buffer
    for(int i = 0; i < nsegments; i++) {
        struct Segment *old = &(segments_front[i]);
        struct Segment *new = &(segments_back[i]);
        new->target_z = old->target_z;
        new->rigidity = old->rigidity;
        new->left = 0;
        new->right = 0;
        
        // calculate physical deformations
        double old_area = 1 / old->z;
        double target_area = 1 / new->target_z;
        double delta = target_area - old_area;
        double new_area = old_area + delta * PHYSICAL_DAMPING;
        if (new_area < 0) new_area = MIN_AREA;
        new->z = 1 / new_area;
    }

    // process each segment
    for(int i = 0; i < nsegments; i++) {
        struct Segment *old = &(segments_front[i]);
        struct Segment *new = &(segments_back[i]);

        // physical compression of the tract walls due to sound pressure
        double area = 1/new->z;

        // process audio moving right (toward lips)
        // if i == 0 then this is at the glottis
        if(i == 0) {
            // make the glottis reflect all sound
            // also mix in the glottal source
            // normalize source for drain impedence
            double gamma = 1-reflection(DRAIN_Z, old->z);
            new->right += old->left * (1-damping) + glottal_source * gamma + diaphram_pressure;
        } else {
            // otherwise the new right moving energy is right moving energy to the old left
            struct Segment *old_left = &(segments_front[i-1]);
            struct Segment *new_left = &(segments_back[i-1]);
            double gamma = reflection(old_left->z, old->z);

            jack_default_audio_sample_t reflection = old_left->right * gamma;
            new->right += old_left->right - reflection;
            new_left->left += reflection * (1-damping);
            
            // frication
            // due to wind hitting obstruction (increase in impedence)
            double velocity = reflection;
            if (velocity < 0) velocity = 0;
            new_left->left += FRICATION * velocity * noise();

            // physical compression of the tract walls due to sound pressure
            //double tmp = area;
            area += reflection * (1-old->rigidity);
            //printf("%f->%f, %f, %f\n", tmp, area, reflection, old->rigidity);
        }

        // process audio moving left (towarard glottis)
        if(i == nsegments-1) {
            // the new left moving energy at the lips is the reflection from the opening
            double gamma = reflection(old->z, DRAIN_Z);
            jack_default_audio_sample_t reflection = old->right * gamma;
            drain = old->right - reflection;
            new->left += reflection * (1-damping);

            // physical compression of the tract walls due to sound pressure
            area += reflection * (1-old->rigidity);

        } else {
            // otherwise the new left moving energy is left moving energy to the old right
            struct Segment *old_right = &(segments_front[i+1]);
            struct Segment *new_right = &(segments_back[i+1]);
            double gamma = reflection(old_right->z, old->z);
            jack_default_audio_sample_t reflection = old_right->left * gamma;
            new->left += old_right->left - reflection;
            new_right->right += reflection * (1-damping);

            // frication
            // due to wind hitting obstruction (increase in impedence)
            double velocity = reflection;
            if (velocity < 0) velocity = 0;
            new_right->right += FRICATION * velocity * noise();

            // physical compression of the tract walls due to sound pressure
            area += reflection * (1-old->rigidity);
        }

        // physical compression of the tract walls due to sound pressure
        if (area < 0) area = MIN_AREA;
        new->z = 1/area;
    }

    // swap waveguide buffers
    swap_buffers();

    // update current phoneme torward target phoneme
    current_phoneme.tongue_position +=
        (target_phoneme->tongue_position - current_phoneme.tongue_position) * interpolation_drag;
    current_phoneme.tongue_height +=
        (target_phoneme->tongue_height - current_phoneme.tongue_height) * interpolation_drag;
    current_phoneme.lips_roundedness +=
        (target_phoneme->lips_roundedness - current_phoneme.lips_roundedness) * interpolation_drag;
    update_shape(0);

#ifdef DEBUG_TRACT
    // list the state of all the segments
    printf("\n\nDEBUG:\n\n");
    debug_tract(segments_front, segments_back);
#endif

    // return the output of the mouth
    return drain;
}

// maps a midi controller value to a given range
double map2range(uint8_t value, double min, double max) {
    return min + (max - min) * (value / 127.0);
}

// callback to process a single chunk of audio
int process(jack_nframes_t nframes, void *arg) {

    // the audio in buffer and the audio out buffer
    jack_default_audio_sample_t *in, *out;
    in = jack_port_get_buffer(input_port, nframes);
    out = jack_port_get_buffer(output_port, nframes);

    // get midi events
    uint8_t *midi_port_buffer = jack_port_get_buffer(midi_input_port, nframes);
    jack_midi_event_t event;
    jack_nframes_t event_count = jack_midi_get_event_count(midi_port_buffer);
    for(int i = 0; i < event_count; i++) {
        jack_midi_event_get(&event, midi_port_buffer, i);
        uint8_t type = event.buffer[0] & 0xf0;
        uint8_t chan = event.buffer[0] & 0x0f;

        // control signal
        if(type == 0xb0) {
            uint8_t id = event.buffer[1];
            uint8_t value = event.buffer[2];
            //printf("  midi control change event: 0x%x, 0x%x\n", id, value);

            if(id==CONTROLLER_TRACT_LENGTH) {
                double desired_length = map2range(value, CONTROLLER_TRACT_LENGTH_MIN, CONTROLLER_TRACT_LENGTH_MAX);
                resize_tract(desired_length);
                printf("setting tract length to desired %2.2fcm...actually got %2.2fcm\n", desired_length, tract_length);
            }
            else if(id==CONTROLLER_TONGUE_HEIGHT) {
                //ambient_phoneme.tongue_height = map2range(value, 0, 0.9);
                ambient_phoneme.tongue_height = map2range(value, 0, 1);
                //update_shape(1);
                printf("setting ambient tongue height to %2.2f%%..\n", ambient_phoneme.tongue_height*100);
            }
            else if(id==CONTROLLER_TONGUE_POSITION) {
                ambient_phoneme.tongue_position = map2range(value, 0, 1);
                //update_shape(1);
                printf("setting ambient tongue frontness to %2.2f%%..\n", ambient_phoneme.tongue_position*100);
            }
            else if(id==CONTROLLER_LIPS_ROUNDEDNESS) {
                //ambient_phoneme.lips_roundedness = map2range(value, 0, 0.9);
                ambient_phoneme.lips_roundedness = map2range(value, 0, 1);
                //update_shape(1);
                printf("setting ambient lips roundedness to %2.2f%%..\n", ambient_phoneme.lips_roundedness*100);
            }
            else if(id==CONTROLLER_DRAG) {
                interpolation_drag = map2range(value, DRAG_MIN, DRAG_MAX);
                printf("setting interpolation drag to %.5f..\n", interpolation_drag);
            }
            else if(id==CONTROLLER_PRESSURE) {
                diaphram_pressure = map2range(value, MIN_DIAPHRAM_PRESSURE, MAX_DIAPHRAM_PRESSURE);
                printf("setting continuous air pressure from lungs to %.3f..\n", diaphram_pressure);
            }
            else if(id==CONTROLLER_DAMPING) {
                damping = map2range(value, MIN_DAMPING, MAX_DAMPING);
                printf("setting damping to %.3f..\n", damping);
            }
        }
        else if(type == 0x80 && chan == PHONEME_CHANNEL) {
            //uint8_t note = event.buffer[1];
            //uint8_t velocity = event.buffer[2];
            //printf("  [chan %02d] midi note OFF: 0x%x, 0x%x\n", chan, note, velocity);
            //struct Phoneme *phoneme = get_mapped_phoneme(note);
            //if (target_phoneme == phoneme) {
            //    // TODO: a stack of notes to return to
            //    target_phoneme = &ambient_phoneme;
            //}
        }
        else if(type == 0x90 && chan == PHONEME_CHANNEL) {
            uint8_t note = event.buffer[1];
            uint8_t velocity = event.buffer[2];
            printf("  [chan %02d] midi note ON:  0x%x, 0x%x\n", chan, note, velocity);
            //target_phoneme = get_mapped_phoneme(note);
            ambient_phoneme = *get_mapped_phoneme(note);

            //// TMP: insert plosive transient
            //if(note == 0x30) {
            //    segments_front[nsegments-1].right += diaphram_pressure;
            //}
        }
    }

    // simply copying for now lol
    //memcpy(out, in, sizeof(jack_default_audio_sample_t) * nframes);

    for(int i = 0; i < nframes; i++) {

        // glottal source sample
        jack_default_audio_sample_t source = in[i];

        // run the tract with the source sample and get the tract output
        out[i] = run_tract(source);
    }

    return 0;
}

// callback if jack shuts down
void jack_shutdown(void *arg) {
    exit(1);
}

int main(int argc, char **argv) {

    // create jack client
    jack_status_t status;
    client = jack_client_open("nancealoid", JackNullOption, &status, NULL);
    if(client == NULL) {
        fprintf(stderr, "could not create jack client\nstatus = 0x%2.0x\n", status);
        if(status & JackServerFailed) {
            fprintf(stderr, "unable to connect to jack server\n");
        }
        exit(1);
    }
    if(status & JackServerStarted) {
        fprintf(stderr, "jack server started\n");
    }

    // set jack callbacks
    jack_set_process_callback(client, process, 0);
    jack_on_shutdown(client, jack_shutdown, 0);

    // create in and out port
    midi_input_port = jack_port_register(client, "nancealoid control", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    input_port = jack_port_register(client, "glottal source", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    output_port = jack_port_register(client, "vocal tract output", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if((input_port == NULL) || (output_port == NULL)) {
        fprintf(stderr, "could not create jack ports...\n");
        exit(1);
    }

    // setup the vocal tract
    ambient_phoneme.tongue_height = 0;
    ambient_phoneme.tongue_position = 0.5;
    ambient_phoneme.lips_roundedness = 0;
    target_phoneme = &ambient_phoneme;
    current_phoneme = ambient_phoneme;
    interpolation_drag = DEFAULT_INTERPOLATION_DRAG;
    diaphram_pressure = 0;
    damping = DEFAULT_DAMPING;
    init_tract(TRACT_LENGTH);

    // go dude go
    if(jack_activate(client)) {
        fprintf(stderr, "couldnt activate jack client lol\n");
        exit(1);
    }

    // wait........ FOREVER...... (nah just til user say so)
    sleep(-1);
    free_tract();
    jack_client_close(client);
    return 0;
}
