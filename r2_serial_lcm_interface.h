// serial_lcm_interface.h
// Class and generic methods for an LCM interface to a serial device
//

#ifndef R2_SLI_H
#define R2_SLI_H

#include <stdio.h> // for fprintf, stderr
#include <stdint.h> // for int64_t
#include <termios.h> // for serial port
#include <time.h> // for nanosleep
#include <unistd.h> // for getpid

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
    struct r2_serial_port * sio;
    lcm_t * lcm;
    management_control_t_subscription_t * management_control_subscription; 
};

typedef void ( * r2_sli_publisher )( struct r2_sli * self, const char * data,
        const int64_t epoch_usec );

struct r2_sli * r2_sli_create( const char * device, const speed_t baud_rate,
    const size_t buffer_size, const char * provider );

static void r2_sli_destroy( struct r2_sli * self );

static void r2_sli_management_control_handler( const lcm_recv_buf_t *rbuf,
        const char * channel, const management_control_t * msg,
        void * user);

static void r2_sli_raw_serial_line_publisher( lcm_t * lcm, const char * channel,
        const char * line, const int64_t epoch_usec );

static void r2_sli_stream( struct r2_sli * self, void * data_splitter,
        void * publisher, const int64_t period );

static void r2_sli_stream_line( struct r2_sli * self, 
        r2_sli_publisher publisher, const int64_t period_usec );

#endif // R2_SLI_H

#ifndef R2_SLI_I
#define R2_SLI_I

struct r2_sli * r2_sli_create(const char * device, speed_t baud_rate,
    size_t buffer_size, const char * provider)
{
    struct r2_sli * self = calloc(1, sizeof(struct r2_sli));

    if( !self ) {
        fprintf(stderr, "Could not allocate memory for serial-LCM interface.");
        return NULL;
    }

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

    self->management_control_subscription = management_control_t_subscribe( 
            self->lcm, "WETLabs.BB2FL.process_control", 
            &r2_sli_management_control_handler, NULL);

    return self;
}

void r2_sli_destroy( struct r2_sli * self)
{
    r2_serial_port_destroy(self->sio);

    // unsubscribe from the standard subscriptions
    management_control_t_unsubscribe(self->lcm,
            self->management_control_subscription);

    lcm_destroy(self->lcm);
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

// TODO: evaluate how much this affects performance vs. keeping the msg allocated
void r2_sli_raw_serial_line_publisher( lcm_t * lcm, const char * channel,
        const char * line, const int64_t epoch_usec )
{
    // Need to make explicit copies if instantiating message types within the
    // publisher, otherwise you can't guarantee const args will still be the 
    // same after the function completes.
    raw_string_t msg = {
        .epoch_usec = epoch_usec,
        .text = strdup( line ), // because I don't want to change the data
    };
    raw_string_t_publish( lcm, channel, &msg );
}

/*** Streaming methods ***/

void r2_sli_stream( struct r2_sli * self, void * data_splitter, 
        void * publisher, const int64_t period )
{
    fprintf( stderr, "r2_sli_stream not yet implemented\n" );
    fprintf( stderr, "look at r2_sli_stream_line in the meantime\n" );
}

void r2_sli_stream_line( struct r2_sli * self, r2_sli_publisher publisher,
        const int64_t period_usec )
{
    // TODO: Revise to simply call r2_sli_stream.
    const struct timespec period = { 0, 950 * period_usec };
    fd_set rfds; // file descriptors to check for readability with select
    const int sfd = self->sio->fd;
    const int lfd = lcm_get_fileno(self->lcm);
    const int maxfd = sfd > lfd ? sfd + 1 : lfd + 1;
    int64_t epoch_usec = 0;
    int run = 1;

    size_t line_size = self->sio->buffer->size;
    char line[line_size];
    memset(line, '\0', line_size);

    while( run ) {
        // add both i/o file descriptors to the fdset for select
        FD_ZERO(&rfds);
        FD_SET(sfd, &rfds);
        FD_SET(lfd, &rfds);

        if( -1 != select( maxfd, &rfds, NULL, NULL, NULL ) ) {
            if( FD_ISSET( sfd, &rfds ) ) { // check for serial input first
                r2_buffer_fill( self->sio->buffer, sfd );
                while( r2_buffer_get_any_line( self->sio->buffer, line,
                            line_size ) ) {
                    epoch_usec = r2_epoch_usec_now(); // update timestamp
                    publisher( self, line, epoch_usec ); // process & publish
                }
            }
            if( FD_ISSET( lfd, &rfds ) ) { // then check for LCM input
                lcm_handle( self->lcm ); // handle all LCM input with handlers
            }
            if( r2_epoch_usec_now() - epoch_usec < period_usec ) {
                nanosleep( &period, NULL ); // sleep if you recently read a line
            }
        } else {
            perror( "select()" );
            if( errno == EINTR ) {
                run = 1; // Keep going if interrupted by a system call.
                // TODO: Check and handle signals instead.
                //       SIGTERM could be used to terminate cleanly.
            } else {
                run = 0;
            }
        }
    }
}

/*** Polling methods ***/

#endif // R2_SLI_I
