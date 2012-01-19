#ifndef __NETLOG__
#define __NETLOG__

#define CONNECT_PROBE_FAILED -1
#define ACCEPT_PROBE_FAILED -2
#define SHUTDOWN_PROBE_FAILED -3
#define BIND_PROBE_FAILED -4 

/* Change to non zero value (i.e. 1) if you wish to probe 
 * the binding of UDP sockets.
 */

#define PROBE_UDP 1

/* Change to zero value (0) if you wish to not probe 
 * the close system call for the sockets.
 */

#define PROBE_CONNECTION_CLOSE 1

/* This symbolic constant defines the maximum of kretprobe 
 * instances that can run simultaneously.
 */

#define MAX_ACTIVE 100

/*Set to non-zero value in order to compile the whitelisting code*/

#define WHITELISTING 1


/*Max lenght of the execution path of the process to be whitelisted.
 *Must be less or equal to MAX_ABSOLUTE_EXEC_PATH, defined in the
 *whitelist.h header file.
 */

#define MAX_EXEC_PATH 64

/*Process names to be whitelisted */

#define NO_WHITELISTS 1

const char procs_to_whitelist[NO_WHITELISTS][MAX_EXEC_PATH] =
{
		"/usr/sbin/sshd", //whitelisting sshd just for debug purposes
};

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Panos Sakkos <panos.sakkos@cern.ch>");
MODULE_DESCRIPTION("Logs process name, pid, uid, source ip, source port,\
		    destination ip and destination port for every TCP connection.\
		    Also logs connection close and UDP binds");

int __init plant_probes(void);
void __exit unplant_probes(void);

module_init(plant_probes);
module_exit(unplant_probes);

#endif