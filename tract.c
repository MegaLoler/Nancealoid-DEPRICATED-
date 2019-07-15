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
#include <jack/jack.h>

#define SPEED_OF_SOUND 34300    // cm per second
#define TRACT_LENGTH 17.5       // desired tract length in cm
#define DRAIN_Z 0.1           // acoustic impedance at the opening of the lips

//#define DEBUG

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

    // allocate memory for the segment of the waveguide
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
        f->z = 1;
        f->left = 0;
        f->right = 0;
        // init back buffer
        b->z = 1;
        b->left = 0;
        b->right = 0;
    }

#ifdef DEBUG
    // test impulse
    segments_front->right = 1;
#endif

    // test set the tract shape
    segments_front[nsegments-2].z = 0.1;

    // print some INTERESTING INFORMATION,
    printf("rate = %ihz\n", rate);
    printf("desired tract length = %fcm\n", desired_length);
    printf("actual tract length = %fcm\n", tract_length);
    printf("unit length = %fcm\n", unit_length);
    printf("num waveguide segments = %i\n", nsegments);
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

#ifdef DEBUG
    // list the state of all the segments
    printf("\n\nDEBUG:\n\n");
    debug_tract(segments_front, segments_back);
#endif

    // return the output of the mouth
    return drain;
}

// callback to process a single chunk of audio
int process(jack_nframes_t nframes, void *arg) {

    // the in buffer and the out buffer
    jack_default_audio_sample_t *in, *out;
    in = jack_port_get_buffer(input_port, nframes);
    out = jack_port_get_buffer(output_port, nframes);

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
    input_port = jack_port_register(client, "glottal source", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    output_port = jack_port_register(client, "vocal tract output", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if((input_port == NULL) || (output_port == NULL)) {
        fprintf(stderr, "could not create jack ports...\n");
        exit(1);
    }

    // setup the vocal tract
    init_tract(TRACT_LENGTH);

    // go dude go
    if(jack_activate(client)) {
        fprintf(stderr, "couldnt activate jack client lol\n");
        exit(1);
    }

    // wait........ FOREVER...... (nah just til user say so)
    sleep(-1);
    jack_client_close(client);
    return 0;
}