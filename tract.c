/*
 * nancealoid tract
 *
 * this program simulates a vocal tract using a 1d digital waveguide
 * takes an audio input source and filters it through the tract
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

// midi controllers for different functions
#define CONTROLLER_TONGUE_POSITION 0x15
#define CONTROLLER_TONGUE_HEIGHT 0x16
#define CONTROLLER_LIPS_ROUNDEDNESS 0x17
#define CONTROLLER_TRACT_LENGTH 0x18

// controller ranges
#define CONTROLLER_TRACT_LENGTH_MIN 8
#define CONTROLLER_TRACT_LENGTH_MAX 24

// tongue start and stop (percentage of tract)
#define TONGUE_BACK 0.2
#define TONGUE_FRONT 0.9

// how fast to sitch between phonemes
#define INTERPOLATION_DRAG 0.0007

//#define DEBUG_TRACT

jack_port_t *midi_input_port;
jack_port_t *input_port;
jack_port_t *output_port;
jack_client_t *client;

// a waveguide segment
struct Segment {
    double z; // acoustic impedence at this segment (inverse of cross sectional area (i think lol))
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

// phoneme to return to
// controlled freely by midi control signals
struct Phoneme ambient_phoneme;

// target phoneme
// point it to what you want the phoneme to be
// simulation will interpolate towards it
struct Phoneme *target_phoneme;

// represents the ACTUAL CURRENT INSTANT shape of the mouth
struct Phoneme current_phoneme;

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
void update_shape() {
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
            s->z = THROAT_Z;
        } else if (i >= stop) {
            // front of mouth
            s->z = 1 / (1 - current_phoneme.lips_roundedness + 0.001) * NEUTRAL_Z;
        } else {
            // tongue
            double unit_pos = (i - start) / (double)(ntongue - 1);
            double phase = unit_pos - current_phoneme.tongue_position;
            double value = cos(phase * M_PI / 2) * current_phoneme.tongue_height;
            double unit_area = 1 - value;
            s->z = 1 / (unit_area + 0.001) * NEUTRAL_Z;
        }
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
        f->left = 0;
        f->right = 0;
        // init back buffer
        b->z = NEUTRAL_Z;
        b->left = 0;
        b->right = 0;
    }

#ifdef DEBUG_TRACT
    // test impulse
    segments_front->right = 1;
#endif

    // test set the tract shape
    //segments_front[nsegments-2].z = 10/NEUTRAL_Z;

    // init the tract shape
    update_shape();

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
        printf("SEG#%02d:\tZ=%2.2f\t\tL=%2.2f\tR=%2.2f\t\tL=%2.2f\tR=%2.2f\n", i, f.z, f.left, f.right, b.left, b.right);
    }
}

// calculate a reflection coefficient
// given source impedence and target impedence
double reflection(double source_z, double target_z) {
    return (target_z - source_z) / (target_z + source_z);
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
        new->z = old->z;
        new->left = 0;
        new->right = 0;
    }

    // process each segment
    for(int i = 0; i < nsegments; i++) {
        struct Segment *old = &(segments_front[i]);
        struct Segment *new = &(segments_back[i]);

        // process audio moving right (toward lips)
        // if i == 0 then this is at the glottis
        if(i == 0) {
            // make the glottis reflect all sound with no loss
            // also mix in the glottal source
            // normalize source for drain impedence
            double gamma = 1-reflection(DRAIN_Z, old->z);
            new->right += old->left + glottal_source * gamma;
        } else {
            // otherwise the new right moving energy is right moving energy to the old left
            struct Segment *old_left = &(segments_front[i-1]);
            struct Segment *new_left = &(segments_back[i-1]);
            double gamma = reflection(old_left->z, old->z);
            jack_default_audio_sample_t reflection = old_left->right * gamma;
            new->right += old_left->right - reflection;
            new_left->left += reflection;
        }

        // process audio moving left (towarard glottis)
        if(i == nsegments-1) {
            // the new left moving energy at the lips is the reflection from the opening
            double gamma = reflection(old->z, DRAIN_Z);
            jack_default_audio_sample_t reflection = old->right * gamma;
            drain = old->right - reflection;
            new->left += reflection;
        } else {
            // otherwise the new left moving energy is left moving energy to the old right
            struct Segment *old_right = &(segments_front[i+1]);
            struct Segment *new_right = &(segments_back[i+1]);
            double gamma = reflection(old_right->z, old->z);
            jack_default_audio_sample_t reflection = old_right->left * gamma;
            new->left += old_right->left - reflection;
            new_right->right += reflection;
        }
    }

    // swap waveguide buffers
    swap_buffers();

    // update current phoneme torward target phoneme
    current_phoneme.tongue_position +=
        (target_phoneme->tongue_position - current_phoneme.tongue_position) * INTERPOLATION_DRAG;
    current_phoneme.tongue_height +=
        (target_phoneme->tongue_height - current_phoneme.tongue_height) * INTERPOLATION_DRAG;
    current_phoneme.lips_roundedness +=
        (target_phoneme->lips_roundedness - current_phoneme.lips_roundedness) * INTERPOLATION_DRAG;
    update_shape();

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
        uint8_t type = event.buffer[0];

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
                ambient_phoneme.tongue_height = map2range(value, 0, 0.9);
                //update_shape();
                printf("setting ambient tongue height to %2.2f%%..\n", ambient_phoneme.tongue_height*100);
            }
            else if(id==CONTROLLER_TONGUE_POSITION) {
                ambient_phoneme.tongue_position = map2range(value, 0, 1);
                //update_shape();
                printf("setting ambient tongue frontness to %2.2f%%..\n", ambient_phoneme.tongue_position*100);
            }
            else if(id==CONTROLLER_LIPS_ROUNDEDNESS) {
                ambient_phoneme.lips_roundedness = map2range(value, 0, 0.9);
                //update_shape();
                printf("setting ambient lips roundedness to %2.2f%%..\n", ambient_phoneme.lips_roundedness*100);
            }
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
    client = jack_client_open("nancealoid tract", JackNullOption, &status, NULL);
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
    midi_input_port = jack_port_register(client, "tract control", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
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
