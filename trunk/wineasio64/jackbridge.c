/** @file simple_client.c
 *
 * @brief This simple client demonstrates the basic features of JACK
 * as they would be used by many applications.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#include <semaphore.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <jack/jack.h>

jack_port_t *input_port[INPUT_PORTS];
jack_port_t *output_port[OUTPUT_PORTS];
jack_client_t *client;

sem_t *sem1, *sem2;
InfoBlock *info;
float *in;
float *out;

/* a simple state machine for this client */
volatile enum {
	Init,
	Run,
	Exit
} client_state = Init;

/**
 * The process callback for this JACK application is called in a
 * special realtime thread once for each audio cycle.
 *
 * This client follows a simple rule: when the JACK transport is
 * running, copy the input port to the output.  When it stops, exit.
 */
int
process (jack_nframes_t nframes, void *arg)
{
        int i;
        struct sched_param param;
        jack_transport_state_t ts;
        jack_position_t jack_position_info;

        ts = jack_transport_query(client, &jack_position_info);
        info->transport_rolling = (ts == JackTransportRolling) || (ts == JackTransportLooping);
        info->frame = jack_position_info.frame;

        if (info->running == 1) {

           for (i=0; i<INPUT_PORTS; i++) {
               memcpy(&in[i*info->buffer_frames], 
                      jack_port_get_buffer (input_port[i], nframes),
                      sizeof (jack_default_audio_sample_t) * nframes);
           }

           sem_post(sem1);
           sem_wait(sem2);
           
           for (i=0; i<OUTPUT_PORTS; i++) {
               memcpy(jack_port_get_buffer (output_port[i], nframes),
                      &out[i*info->buffer_frames],
                      sizeof (jack_default_audio_sample_t) * nframes);
           }
        }
        else {

           for (i=0; i<OUTPUT_PORTS; i++) {
               memset(jack_port_get_buffer (output_port[i], nframes),
                      sizeof (jack_default_audio_sample_t) * nframes, 0);
           }
           sched_getparam(0, &param);
           info->priority = param.__sched_priority;

        }

	return 0;      
}

/**
 * JACK calls this shutdown_callback if the server ever shuts down or
 * decides to disconnect the client.
 */
void
jack_shutdown (void *arg)
{
	exit (1);
}

int
main (int argc, char *argv[])
{
        int handle;
	int i;
	const char **ports;
	const char *client_name;
	const char *server_name = NULL;
        char name[32];
	jack_options_t options = JackNullOption;
	jack_status_t status;

	if (argc >= 2) {		/* client name specified? */
		client_name = argv[1];
		if (argc >= 3) {	/* server name specified? */
			server_name = argv[2];
			options |= JackServerName;
		}
	} else {			/* use basename of argv[0] */
		client_name = strrchr(argv[0], '/');
		if (client_name == 0) {
			client_name = argv[0];
		} else {
			client_name++;
		}
	}

	/* open a client connection to the JACK server */

	client = jack_client_open (client_name, options, &status, server_name);
	if (client == NULL) {
		fprintf (stderr, "jack_client_open() failed, "
			 "status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		exit (1);
	}
	if (status & JackServerStarted) {
		fprintf (stderr, "JACK server started\n");
	}
	if (status & JackNameNotUnique) {
		client_name = jack_get_client_name(client);
		fprintf (stderr, "unique name `%s' assigned\n", client_name);
	}

	/* tell the JACK server to call `process()' whenever
	   there is work to be done.
	*/

	jack_set_process_callback (client, process, 0);

	/* tell the JACK server to call `jack_shutdown()' if
	   it ever shuts down, either entirely, or if it
	   just decides to stop calling us.
	*/

	jack_on_shutdown (client, jack_shutdown, 0);

	/* display the current sample rate. 
	 */

	printf ("engine sample rate: %" PRIu32 "\n",
		jack_get_sample_rate (client));

	/* create two ports */

        for (i=0; i<INPUT_PORTS; i++) {
            snprintf(name, 32, "input-%d", i);
	    input_port[i] = jack_port_register (client, name,
					 JACK_DEFAULT_AUDIO_TYPE,
					 JackPortIsInput, 0);
        }

        for (i=0; i<OUTPUT_PORTS; i++) {
            snprintf(name, 32, "output-%d", i);
            output_port[i] = jack_port_register (client, name,
                                         JACK_DEFAULT_AUDIO_TYPE,
                                         JackPortIsOutput, 0);
        }

	/* Tell the JACK server that we are ready to roll.  Our
	 * process() callback will start running now. */

        sem1 = sem_open("wineasio-sem1", O_CREAT | O_RDWR, 0666, 0);
        sem2 = sem_open("wineasio-sem2", O_CREAT | O_RDWR, 0666, 0);

        if ((handle = shm_open("wineasio-info", O_CREAT | O_RDWR, 0666)) == -1)
        {
           printf("failed to open shm info. Is jack client running?\n");
           exit(1);
        }

        ftruncate(handle, sizeof(InfoBlock));
        info = (InfoBlock *)mmap(0, sizeof(InfoBlock),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);

        close(handle);
        
        info->running = 0;
        info->inputs = INPUT_PORTS;
        info->outputs = OUTPUT_PORTS;
        info->buffer_frames = (unsigned int)jack_get_buffer_size(client);
        info->sample_rate = (unsigned int)jack_get_sample_rate(client);

        if ((handle = shm_open("wineasio-buffers", O_CREAT | O_RDWR, 0666)) == -1)
        {
           printf("failed to open shm buffers. Is jack client running?\n");
           exit(2);
        }

        ftruncate(handle, sizeof(float)* info->buffer_frames * (info->inputs + info->outputs));
        in = (float *)mmap(0,
                           sizeof(float) * info->buffer_frames * (info->inputs + info->outputs),
                           PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
        close(handle);

        out = in + info->inputs * info->buffer_frames;

	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
		exit (1);
	}

	/* Connect the ports.  You can't do this before the client is
	 * activated, because we can't make connections to clients
	 * that aren't running.  Note the confusing (but necessary)
	 * orientation of the driver backend ports: playback ports are
	 * "input" to the backend, and capture ports are "output" from
	 * it.
	 */

/*	ports = jack_get_ports (client, NULL, NULL,
				JackPortIsPhysical|JackPortIsOutput);
	if (ports == NULL) {
		fprintf(stderr, "no physical capture ports\n");
		exit (1);
	}

	if (jack_connect (client, ports[0], jack_port_name (input_port))) {
		fprintf (stderr, "cannot connect input ports\n");
	}

	free (ports);
	
	ports = jack_get_ports (client, NULL, NULL,
				JackPortIsPhysical|JackPortIsInput);
	if (ports == NULL) {
		fprintf(stderr, "no physical playback ports\n");
		exit (1);
	}

	if (jack_connect (client, jack_port_name (output_port), ports[0])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	free (ports);
*/
	/* keep running until the transport stops */
        while (client_state != Exit) sleep(1);

        sem_destroy(sem2);
        sem_destroy(sem1);

        shm_unlink("wineasio-info");
        shm_unlink("wineasio-buffer");

	jack_client_close (client);
	exit (0);
}
