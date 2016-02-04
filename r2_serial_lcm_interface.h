// serial_lcm_interface.h
// Class and generic methods for an LCM interface to a serial device
//

#ifndef R2_SLI_H
#define R2_SLI_H

#include <errno.h> // for EXIT_SUCCESS, EXIT_FAILURE
#include <fcntl.h> // for write
#include <signal.h> // for SIGHUP, SIGINT, SIGTERM, sigset_t, sigaction
#include <stdio.h> // for fprintf, stderr
#include <stdint.h> // for int64_t
#include <termios.h> // for serial port
#include <time.h> // for nanosleep
#include <unistd.h> // for getpid, STDOUT_FILENO, STDIN_FILENO

#include <lcm/lcm.h>

// Requires source generated by lcm-gen -c for raw and management lcmtypes
//     raw:        git@bitbucket.org:mbari/raw-lcmtypes.git
//     management: git@bitbucket.org:mbari/process-management-lcmtypes.git
#include "raw_string_t.h"
#include "management_control_t.h"
#include "management_process_t.h"
#include "management_syslog_t.h"

#include "r2_epoch.h"
#include "r2_serial_port.h"

struct r2_sli {
    management_process_t process;
    struct r2_serial_port * sio;
    lcm_t * lcm;
    management_control_t_subscription_t * management_control_subscription;
};

typedef void ( * r2_sli_publisher )( struct r2_sli * self, const char * data,
        const int64_t epoch_usec );

struct r2_sli * r2_sli_create( const char * name,  const char * device,
        const speed_t baud_rate, const size_t buffer_size,
        const char * provider );

static void r2_sli_destroy( struct r2_sli * self );

static int r2_sli_keep_streaming = 1;

static void r2_sli_handle_stop_signal (int signal);

static void r2_sli_management_control_handler( const lcm_recv_buf_t *rbuf,
        const char * channel, const management_control_t * msg,
        void * user);

static void r2_sli_raw_serial_line_publisher( struct r2_sli * self,
        const char * channel, raw_string_t * msg, const char * line,
        const int64_t epoch_usec );

static void r2_sli_stream( struct r2_sli * self, r2_buffer_splitter splitter,
        r2_sli_publisher publisher );

static void r2_sli_stream_line( struct r2_sli * self,
        r2_sli_publisher publisher );

#endif // R2_SLI_H

#ifndef R2_SLI_I
#define R2_SLI_I

struct r2_sli * r2_sli_create( const char * name,  const char * device,
        speed_t baud_rate, size_t buffer_size, const char * provider)
{
    struct r2_sli * self = calloc(1, sizeof(struct r2_sli));

    if( !self ) {
        fprintf(stderr, "Could not allocate memory for serial-LCM interface.");
        return NULL;
    }

    self->process.name = (char *)name;
    self->process.id = getpid();

    self->sio = r2_serial_port_create( device, baud_rate, buffer_size );
    if( !self->sio ) {
        fprintf(stderr, "Could not create serial interface.");
        return NULL;
    }

    self->lcm = lcm_create( provider );
    if( !self->lcm ) {
        fprintf(stderr, "Could not create LCM instance.");
        return NULL;
    }

    char control_channel[strlen( name ) + 5];
    sprintf( control_channel, "%s.ctrl", name );

    self->management_control_subscription = management_control_t_subscribe(
            self->lcm, control_channel, &r2_sli_management_control_handler,
            NULL);

    return self;
}

void r2_sli_destroy( struct r2_sli * self)
{
    printf("Destroying Serial-LCM interface at %"PRId64".\n", 
            r2_epoch_usec_now());

    // TODO: Publish (a few?) messages saying that the daemon is exiting.

    printf("Destroying serial port.\n");
    r2_serial_port_destroy(self->sio);
    printf("Destroyed serial port.\n");

    printf("Unsubscribing from the standard LCM subscriptions.\n");
    management_control_t_unsubscribe(self->lcm,
            self->management_control_subscription);
    printf("Unsubscribed from the standard LCM subscriptions.\n");

    printf("Destroying LCM instance.\n");
    lcm_destroy(self->lcm);
    printf("Destroyed LCM instance.\n");

    printf("Destroyed Serial-LCM interface at %"PRId64".\n",
            r2_epoch_usec_now());
}

/*** Signal handlers ***/
static void r2_sli_handle_stop_signal (int signal)
{
    switch (signal) {
        case SIGHUP:
        case SIGINT:
        case SIGTERM:
            write(STDOUT_FILENO, "Caught stop signal.\n", 20);
            r2_sli_keep_streaming = 0;
            break;
        default:
            write(STDERR_FILENO, "Caught non-stop signal.\n", 24);
            r2_sli_keep_streaming = 1;
    }
}

/*** Input handlers ***/

void r2_sli_management_control_handler(const lcm_recv_buf_t *rbuf,
        const char * channel, const management_control_t * msg,
        void * user)
{
    printf("Received message on channel %s: signal=%s\n", channel, msg->signal);
    // TODO: Let incoming signals actually do something!
}


/*** Output publishers ***/

void r2_sli_raw_serial_line_publisher( struct r2_sli * self,
        const char * channel, raw_string_t * msg, const char * line,
        const int64_t epoch_usec )
{
    msg->epoch_usec = epoch_usec;
    msg->text = (char *)line; // cast to explicitly drop const modifier
    raw_string_t_publish( self->lcm, channel, msg );
}

/*** Streaming methods ***/

void r2_sli_stream(struct r2_sli * self, r2_buffer_splitter splitter,
        r2_sli_publisher publisher)
{
    const struct timespec wait_before = { 0, 10 };
    fd_set rfds; // file descriptors to check for readability with select
    const int sfd = self->sio->fd;
    const int lfd = lcm_get_fileno(self->lcm);
    const int maxfd = sfd > lfd ? sfd + 1 : lfd + 1;
    int64_t epoch_usec = 0;

    struct sigaction sa;
    sigset_t unblocked, blocked;

    size_t frame_size = self->sio->buffer->size;
    char frame[frame_size];
    memset(frame, '\0', frame_size);

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGHUP);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &blocked, &unblocked) < 0) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    sa.sa_handler = &r2_sli_handle_stop_signal;
    sigfillset(&sa.sa_mask); // Block every signal during the handler

    if (sigaction(SIGHUP, &sa, NULL) == -1) {
        perror("sigaction cannot handle SIGHUP"); // should not happen
    }
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction cannot handle SIGINT"); // should not happen
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction cannot handle SIGTERM"); // should not happen
    }

    r2_sli_keep_streaming = 1;
    while (r2_sli_keep_streaming) {
        // add both i/o file descriptors to the fdset for select
        FD_ZERO(&rfds);
        FD_SET(sfd, &rfds);
        FD_SET(lfd, &rfds);

        if (-1 != pselect(maxfd, &rfds, NULL, NULL, NULL, &unblocked)) {
            if (FD_ISSET(sfd, &rfds)) { // check for serial input first
                epoch_usec = r2_epoch_usec_now(); // update timestamp
                nanosleep(&wait_before, NULL); // wait for full packet
                r2_buffer_fill(self->sio->buffer, sfd);
                while (splitter(self->sio->buffer, frame, frame_size)) {
                    publisher(self, frame, epoch_usec); // process & publish
                    r2_buffer_fill(self->sio->buffer, sfd); // get moar data
                }
            }
            if (FD_ISSET(lfd, &rfds)) { // then check for LCM input
                lcm_handle(self->lcm); // handle all LCM input with handlers
            }
        } else {
            switch errno {
                case EINTR: // interrupted by a signal
                    break;
                default:
                    perror("pselect");
                    r2_sli_keep_streaming = 0;
            }
        }
    }
}

void r2_sli_stream_line(struct r2_sli * self, r2_sli_publisher publisher)
{
    r2_sli_stream(self, r2_buffer_get_any_line, publisher);
}

/*** Polling methods ***/

#endif // R2_SLI_I
