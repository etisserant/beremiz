/* File generated by Beremiz (PlugGenerate_C method of modbus Plugin instance) */

/*
 * Copyright (c) 2016 Mario de Sousa (msousa@fe.up.pt)
 *
 * This file is part of the Modbus library for Beremiz and matiec.
 *
 * This Modbus library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this Modbus library.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This code is made available on the understanding that it will not be
 * used in safety-critical situations without a full and competent review.
 */

#include "mb_addr.h"
#include "mb_tcp_private.h"
#include "mb_master_private.h"



#define DEF_REQ_SEND_RETRIES 0


#define MODBUS_PARAM_STRING_SIZE 64


  // Used by the Modbus server node
#define MEM_AREA_SIZE 65536
typedef struct{
	    u16		ro_bits [MEM_AREA_SIZE];
	    u16		rw_bits [MEM_AREA_SIZE];
	    u16		ro_words[MEM_AREA_SIZE];
	    u16		rw_words[MEM_AREA_SIZE];
            /* Two flags to count the number of Modbus requests (read and write) we have 
             * successfully received from any remote Modbus master
             * These two flags will be mapped onto located variables
             * so the user's IEC 61131-3 code can check whether we are being
             * polled by a Modbus master.
             * The counters will roll over to 0 upon reaching maximum value.
             */
            u32         flag_write_req_counter;
            u32         flag_read_req_counter;
	} server_mem_t;


/*
 * Beremiz has a program to run on the PLC (Beremiz_service.py)
 * to handle downloading of compiled programs, start/stop of PLC, etc.
 * (see runtime/PLCObject.py for start/stop, loading, ...)
 * 
 * This service also includes a web server to access PLC state (start/stop)
 * and to change some basic confiuration parameters.
 * (see runtime/NevowServer.py for the web server)
 * 
 * The web server allows for extensions, where additional configuration
 * parameters may be changed on the running/downloaded PLC.
 * Modbus plugin also comes with an extension to the web server, through
 * which the basic Modbus plugin configuration parameters may be changed
 *
 * This means that most values in the server_node_t and client_node_t
 * may be changed after the co,piled code (.so file) is loaded into 
 * memory, and before the code starts executing.
 * Since the we will also want to change the host and port (TCP) and the
 * serial device (RTU) at this time, it is best if we allocate memory for
 * these strings that may be overwritten by the web server (i.e., do not use
 * const strings) in the server_node_t and client_node_t structures.
 *
 * The following structure members
 *    - node_addr_t.addr.tcp.host
 *    - node_addr_t.addr.tcp.service  (i.e. the port)
 *    - node_addr_t.addr.rtu.device
 * are all char *, and do not allocate memory for the strings.
 * 
 * We therefore include two generic char arrays, str1 and str2,
 * that will store the above strings, and the C code will initiliaze
 * the node_addre_t.addr string pointers to these strings.
 * i.e., either addr.rtu.device will point to str1,
 *          or
 *        addr.tcp.host and addr.tcp.service 
 *        will point to str1 and str2 respectively
 */
typedef struct{
	    const char *location;
        const char *config_name;
              char  str1[MODBUS_PARAM_STRING_SIZE];
              char  str2[MODBUS_PARAM_STRING_SIZE]; 
	    u8		slave_id;
	    node_addr_t	node_address;
	    int		mb_nd;      // modbus library node used for this server 
	    int		init_state; // store how far along the server's initialization has progressed
            /* entries from this point forward are not statically initialized when the variable is declared */
            /* they will be initialized by the  code itself in the init() function */
	    pthread_t	thread_id;  // thread handling this server
	    server_mem_t	mem_area;
	} server_node_t;


  // Used by the Modbus client node
typedef struct{
	    const char *location;
        const char *config_name;
              char  str1[MODBUS_PARAM_STRING_SIZE];
              char  str2[MODBUS_PARAM_STRING_SIZE]; 
	    node_addr_t	node_address;
	    int		mb_nd;      // modbus library node used for this client
	    int		init_state; // store how far along the client's initialization has progressed
	    u64		comm_period;// period to use when periodically sending requests to remote server
	    int		prev_error; // error code of the last printed error message (0 when no error) 
	    pthread_t   thread_id;  // thread handling all communication for this client node
	    pthread_t	timer_thread_id;  // thread handling periodical timer for this client node
	    pthread_mutex_t mutex;  // mutex to be used with the following condition variable
        pthread_cond_t  condv;  // used to signal the client thread when to start new modbus transactions
        int       execute_req;  /* used, in association with condition variable,  
                                 *   to signal when to send the modbus request to the server
                                 * Note that we cannot simply rely on the condition variable to signal
                                 *   when to activate the client thread, as the call to 
                                 *   pthread_cond_wait() may return without having been signaled!
                                 *   From the manual:
                                 *      Spurious  wakeups  from  the
                                 *      pthread_cond_timedwait() or pthread_cond_wait()  functions  may  occur.
                                 *      Since  the  return from pthread_cond_timedwait() or pthread_cond_wait()
                                 *      does not imply anything about the value of this predicate,  the  predi-
                                 *      cate should be re-evaluated upon such return.
                                 */
        int      periodic_act;  /* (boolen) flag will be set when the client node's thread was activated 
                                 * (by signaling the above condition variable) by the periodic timer.
                                 * Note that this same thread may also be activated (condition variable is signaled)
                                 * by other sources, such as when the user program requests that a specific 
                                 * client MB transation be executed (flag_exec_req in client_request_t)
                                 */
	} client_node_t;


  // Used by the Modbus client plugin
typedef enum {
	    req_input,
	    req_output,
	    no_request		/* just for tests to quickly disable a request */
	} iotype_t;

#define REQ_BUF_SIZE 2000
typedef struct{
	    const char *location;
	    int		client_node_id;
	    u8		slave_id;
	    iotype_t	req_type;
	    u8		mb_function;
	    u16		address;
	    u16		count;
	    int		retries;
	    u8		mb_error_code; // modbus      error code (if any) of last executed request
	    u8		tn_error_code; // transaction error code (if any) of last executed request
	    int		prev_error; // error code of the last printed error message (0 when no error) 
	    struct timespec resp_timeout;
	    u8		write_on_change; // boolean flag. If true => execute MB request when data to send changes
	      // buffer used to store located PLC variables
	    u16		plcv_buffer[REQ_BUF_SIZE];
	      // buffer used to store data coming from / going to server
	    u16		coms_buffer[REQ_BUF_SIZE]; 
	    pthread_mutex_t coms_buf_mutex; // mutex to access coms_buffer[]
          /* boolean flag that will be mapped onto a (BOOL) located variable 
           * (u8 because IEC 61131-3 BOOL are mapped onto u8 in C code! )
           *    -> allow PLC program to request when to start the MB transaction
           *    -> will be reset once the MB transaction has completed
           */
        u8     flag_exec_req;  
          /* flag that works in conjunction with flag_exec_req
           * (does not really need to be u8 as it is not mapped onto a located variable. )
           *    -> used by internal logic to indicate that the client thread 
           *       that will be executing the MB transaction
           *       requested by flag exec_req has already been activated.
           *    -> will be reset once the MB transaction has completed
           */
        u8     flag_exec_started;  
          /* flag that will be mapped onto a (BYTE) located variable 
           * (u8 because the flag is a BYTE! )
           *    -> will store the result of the last executed MB transaction
           *         1 -> error accessing IP network, or serial interface
           *         2 -> reply received from server was an invalid frame
           *         3 -> server did not reply before timeout expired
           *         4 -> server returned a valid Modbus error frame
           *    -> will be reset (set to 0) once this MB transaction has completed sucesfully
           * 
           * In other words, this variable is a copy of tn_error_code, reset after each request attempt completes.
           * We map this copy (instead of tn_error_code) onto a located variable in case the user program decides
           * to overwrite its value and mess up the plugin logic.
           */
        u8      flag_tn_error_code;  
          /* flag that will be mapped onto a (BYTE) located variable 
           * (u8 because the flag is a BYTE! )
           *    -> if flag_tn_error_code is 4, this flag will store the MB error code returned by the MB server in a MB error frame
           *    -> will be reset (set to 0) once this MB transaction has completed succesfully
           * 
           * In other words, this variable is a copy of mb_error_code, reset after each request attempt completes.
           * We map this copy (instead of mb_error_code) onto a located variable in case the user program decides
           * to overwrite its value and mess up the plugin logic.
           */
        u8      flag_mb_error_code;  
	} client_request_t;


/* The total number of nodes, needed to support _all_ instances of the modbus plugin */
#define TOTAL_TCPNODE_COUNT       %(total_tcpnode_count)s
#define TOTAL_RTUNODE_COUNT       %(total_rtunode_count)s
#define TOTAL_ASCNODE_COUNT       %(total_ascnode_count)s

/* Values for instance %(locstr)s of the modbus plugin */
#define MAX_NUMBER_OF_TCPCLIENTS  %(max_remote_tcpclient)s

#define NUMBER_OF_TCPSERVER_NODES %(tcpserver_node_count)s
#define NUMBER_OF_TCPCLIENT_NODES %(tcpclient_node_count)s
#define NUMBER_OF_TCPCLIENT_REQTS %(tcpclient_reqs_count)s

#define NUMBER_OF_RTUSERVER_NODES %(rtuserver_node_count)s
#define NUMBER_OF_RTUCLIENT_NODES %(rtuclient_node_count)s
#define NUMBER_OF_RTUCLIENT_REQTS %(rtuclient_reqs_count)s

#define NUMBER_OF_ASCIISERVER_NODES %(ascserver_node_count)s
#define NUMBER_OF_ASCIICLIENT_NODES %(ascclient_node_count)s
#define NUMBER_OF_ASCIICLIENT_REQTS %(ascclient_reqs_count)s

#define NUMBER_OF_SERVER_NODES (NUMBER_OF_TCPSERVER_NODES + \
                                NUMBER_OF_RTUSERVER_NODES + \
                                NUMBER_OF_ASCIISERVER_NODES)

#define NUMBER_OF_CLIENT_NODES (NUMBER_OF_TCPCLIENT_NODES + \
                                NUMBER_OF_RTUCLIENT_NODES + \
                                NUMBER_OF_ASCIICLIENT_NODES)

#define NUMBER_OF_CLIENT_REQTS (NUMBER_OF_TCPCLIENT_REQTS + \
                                NUMBER_OF_RTUCLIENT_REQTS + \
                                NUMBER_OF_ASCIICLIENT_REQTS)


/*initialization following all parameters given by user in application*/

static client_node_t		client_nodes[NUMBER_OF_CLIENT_NODES] = {
%(client_nodes_params)s
};


static client_request_t	client_requests[NUMBER_OF_CLIENT_REQTS] = {
%(client_req_params)s
};


static server_node_t		server_nodes[NUMBER_OF_SERVER_NODES] = {
%(server_nodes_params)s
}
;

/*******************/
/*located variables*/
/*******************/

%(loc_vars)s

