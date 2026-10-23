// UDP discovery: answers "M99999" with the printer's identity.
#ifndef GKSDCP_DISCOVERY_H
#define GKSDCP_DISCOVERY_H
// Bind UDP port and register with loop_add_fd (call before loop_run). 0 ok / -1.
int discovery_open(int port);
#endif
