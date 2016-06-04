#include "rdma_portal.h"

rdma_session* rdma_portal::accept(int port,struct init_params *params) 
{
	sockfd = open_server(18185);

	struct rdma_context *ctx;
	if(!params) 
		params = (struct init_params*)calloc(1,sizeof *params);
	params->sockfd = sockfd;
	ctx = rdma_socket_p(NULL,port,1,params);
	return new rdma_session(ctx,params);
} 
