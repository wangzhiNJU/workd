#ifndef _GNU_SOURCE
#define  _GNU_SOURCE
#endif

#ifndef RDMA_SOCKET_C
#define RDMA_SOCKET_C 

#include <stdio.h>
#include "rdma_socket.h"
#include "huge_page.c"
struct rdma_context *init_rdma_context(struct ibv_device *ib_dev,
					    int rx_depth, int port,
					    int use_event, int is_server){
	struct rdma_context *ctx;
	
	ctx = (struct rdma_context*)calloc(1, sizeof *ctx);
	if(!ctx) {
		return NULL;
	}
	
	ctx->id = 0;
	
	
	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_ctx;
	}
	
	if (ibv_query_port(ctx->context, 1, &ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return NULL;
	}
	ctx->ib_port = 1;
	if(ctx->portinfo.state != IBV_PORT_ACTIVE) {
		if (ibv_query_port(ctx->context, 2, &ctx->portinfo)) {
			fprintf(stderr, "Couldn't get port info\n");
			return NULL;
		}
		if(ctx->portinfo.state != IBV_PORT_ACTIVE) {
			fprintf(stderr,"All Port Down!\n");
			return NULL;
		}
		ctx->ib_port = 2;
	}
	port = ctx->ib_port;
	if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			goto clean_device;
		}
	} 
	else {
		ctx->channel = NULL;
	}
	
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}

	ctx->recv_cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
				ctx->channel, 0);
	ctx->send_cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
				NULL, 0);
	if (!ctx->recv_cq || !ctx->send_cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_pd;
	}
	
	{
		struct ibv_qp_init_attr attr = {};
		attr.send_cq = ctx->send_cq;
		attr.recv_cq = ctx->recv_cq;
		attr.cap.max_send_wr = 10000;		
		attr.cap.max_recv_wr = rx_depth;		
		attr.cap.max_send_sge = 1;		
		attr.cap.max_recv_sge = 1;		
		attr.qp_type = IBV_QPT_RC;

		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}
	}
	
	{
		struct ibv_qp_attr attr = {};
		attr.qp_state = IBV_QPS_INIT;
		attr.pkey_index = 0;
		attr.port_num = port;
		attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
		

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
	}
	
	return ctx;
	
clean_qp:
	ibv_destroy_qp(ctx->qp);

clean_cq:
	ibv_destroy_cq(ctx->send_cq);
	ibv_destroy_cq(ctx->recv_cq);

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_device:
	ibv_close_device(ctx->context);

clean_ctx:	
	free(ctx);

	return NULL;
}


struct info_con *rdma_client_exch_dest(struct rdma_context* ctx,const char *servername, int port,
						 const struct info_con *my_info)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = /*SOCK_STREAM*/0;

	char *service;
	int sockfd = -1;
	int n;

	//fixxxx char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	char msg[sizeof "00000000:00000000:00000000:0000000000000000:00000000:00000000000000000000000000000000"];
	struct info_con *remote_info = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}
	

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}	

	gid_to_wire_gid(&my_info->gid, gid);
	sprintf(msg, "%08x:%08x:%08x:%016lu:%08x:%s", my_info->lid, my_info->qpn, my_info->psn, my_info->addr, my_info->rkey, gid);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client read");
		fprintf(stderr, "Couldn't read remote address\n");
		goto out;
	}

	write(sockfd, "done", sizeof "done");

	remote_info = (struct info_con*)malloc(sizeof *remote_info);
	if (!remote_info)
		goto out;
	sscanf(msg, "%x:%x:%x:%lu:%x:%s", &remote_info->lid, &remote_info->qpn, &remote_info->psn, &remote_info->addr, &remote_info->rkey, gid);
	wire_gid_to_gid(gid, &remote_info->gid);


	ctx->fd = sockfd;
	printf(" fd :\n ",ctx->fd);
out:
	return remote_info;
}
	
int rdma_connect_ctx(struct rdma_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct info_con *dest, int sgid_idx)
{
	struct ibv_qp_attr attr = {};
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu;
	attr.dest_qp_num = dest->qpn;
	attr.rq_psn = dest->psn;
	attr.max_dest_rd_atomic	= 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global	= 0;
	attr.ah_attr.dlid = dest->lid;
	attr.ah_attr.sl	= sl;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = port;
	

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

int open_server(int port) {
	struct addrinfo *res, *t;
	struct addrinfo hints = {};
	hints.ai_flags    = AI_PASSIVE;
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = /*SOCK_STREAM*/0;

	char *service;
	int n;
	int sockfd = -1, connfd;

	if (asprintf(&service, "%d", port) < 0)
		return -1;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return -1;
	}
	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return -1;
	}

	return sockfd;
}

struct info_con *rdma_server_exch_dest(struct rdma_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct info_con *my_info,
						 int sgid_idx,int sockfd)
{
	//char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	char msg[sizeof "00000000:00000000:00000000:0000000000000000:00000000:00000000000000000000000000000000"];
	struct info_con *remote_info = NULL;
	char gid[33];
	int connfd,n;

	//sockfd = open_server(port);

	listen(sockfd, 1);

	char peerip[18];
    	struct sockaddr_in peeraddr;
    	socklen_t len = sizeof(struct sockaddr_in);;

	connfd = accept(sockfd,  NULL,0);
	
	getpeername(connfd,(struct sockaddr *)&peeraddr, &len);

	const char* host = inet_ntop(AF_INET, &peeraddr.sin_addr, peerip, sizeof(peerip));
	ctx->peer_host = (char*)malloc(strlen(host)+1);
	strcpy(ctx->peer_host,host);
	//close(sockfd);
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}

	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	remote_info = (struct info_con*)malloc(sizeof *remote_info);
	if (!remote_info)
		goto out;

	//sscanf(msg, "%x:%x:%x:%s", &remote_info->lid, &remote_info->qpn, &remote_info->psn, gid);
	sscanf(msg, "%x:%x:%x:%lu:%x:%s", &remote_info->lid, &remote_info->qpn, &remote_info->psn, &remote_info->addr, &remote_info->rkey, gid);
	wire_gid_to_gid(gid, &remote_info->gid);
	fprintf(stdout,"server read : %s\n",gid);
	if (rdma_connect_ctx(ctx, ib_port, my_info->psn, mtu, sl, remote_info, sgid_idx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(remote_info);
		remote_info = NULL;
		goto out;
	}


	gid_to_wire_gid(&my_info->gid, gid);
	fprintf(stdout,"go to send :%s\n",gid);
	sprintf(msg, "%08x:%08x:%08x:%016lu:%08x:%s", my_info->lid, my_info->qpn, my_info->psn, my_info->addr, my_info->rkey, gid);
	if (write(connfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(remote_info);
		remote_info = NULL;
		goto out;
	}

	read(connfd, msg, sizeof msg);
	ctx->fd = connfd;
out:
	return remote_info;
}
	
	
struct rdma_context* create_context(const char* hostname,int iport, int is_server,struct init_params *params){
	//+++++++++++ start of declaration for resource
	struct rdma_context* ctx;
	struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct info_con     my_info;
	struct info_con    *remote_info;
	const char                    *ib_devname = params->ib_devname;//todo
	int                      port = params->port;
	int                      ib_port = params->ib_port;
	enum ibv_mtu		 mtu = params->mtu;
	int                      rx_depth = params->rx_depth;
	int                      use_event = params->use_event;
	int                      sl = 0;
	int			 gidx = params->gidx;
	char			 gid[33];
	int sockfd = params->sockfd;
	//------------- end of declaration for resource
	
	//++++++++++++ start of open ib resource
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return NULL;
	}
	
	if (!ib_devname) {
		ib_dev = *dev_list;
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return NULL;
		}
	} else {
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return NULL;
		}
	}
	fprintf(stdout,"%d %d %d %d\n",rx_depth,ib_port,use_event,is_server);	
	ctx = init_rdma_context(ib_dev, rx_depth, ib_port, use_event, is_server);
	if(!ctx) {
		return NULL;
	}
	ctx->page_size = sysconf(_SC_PAGESIZE);	
	
	if (use_event) {
		if (ibv_req_notify_cq(ctx->recv_cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return NULL;
		}
	}
	

	my_info.lid = ctx->portinfo.lid;
	if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_info.lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return NULL;
	}
	
	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ctx->ib_port, gidx, &my_info.gid)) {
			fprintf(stderr, "Could not get local gid for gid index %d\n", gidx);
			return NULL;
		}
	} else {
		memset(&my_info.gid, 0, sizeof my_info.gid);
	}
	
	my_info.qpn = ctx->qp->qp_num;
	rdma_mem* rw = NULL;
	if(params->mem_size != 0) {
		char* tbuf = (char*)memalign(ctx->page_size,params->mem_size);
		//char* tbuf = (char*)malloc_huge_pages(params->mem_size);
		if(tbuf == NULL) {
			fprintf(stderr," allocate read buffer error!\n");
			return NULL;
		}
		rw = rdma_reg_mr(ctx,tbuf,params->mem_size);	
		if(rw == NULL) {
			fprintf(stderr," allocate read mem error\n");
			free(tbuf);
			return NULL;
		}
	}
	if(rw != NULL) {
		my_info.addr = (uint64_t)rw->buf;
		my_info.rkey = rw->mr->rkey;
		ctx->rw = rw;
	}
	my_info.psn = lrand48() & 0xffffff;
	inet_ntop(AF_INET6, &my_info.gid, gid, sizeof gid);// FOR DEBUG
	printf("  local address:  LID 0x%08x, QPN 0x%08x, PSN 0x%08x, ADDR 0x%016lu, RKEY 0x%08x GID %s\n",
	       my_info.lid, my_info.qpn, my_info.psn, my_info.addr, my_info.rkey, gid);
		   
	if (!is_server) {
		remote_info = rdma_client_exch_dest(ctx,hostname, port, &my_info);
	}
	else {
		remote_info = rdma_server_exch_dest(ctx, ctx->ib_port, mtu, port, sl, &my_info, gidx,sockfd);
	}
	
	if (!remote_info)
		return NULL;
		
	inet_ntop(AF_INET6, &remote_info->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%08x, QPN 0x%08x, PSN 0x%08x, ADDR 0x%016lu, RKEY 0x%08x, GID %s\n",
	       remote_info->lid, remote_info->qpn, remote_info->psn, remote_info->addr, remote_info->rkey, gid);
	ctx->remote_info = remote_info;
	if (!is_server) {
		if (rdma_connect_ctx(ctx, ctx->ib_port, my_info.psn, mtu, sl, remote_info, gidx))
			return NULL;
	}
	ibv_free_device_list(dev_list);
	
	return ctx;
}

struct rdma_context* rdma_socket(const char* hostname,int iport, int is_server){
	struct init_params default_params;
	default_params.port = 18185;
	default_params.ib_port = 1;
	default_params.gidx = -1;
	default_params.use_event = 0;
	default_params.rx_depth = 16000;
	default_params.mtu = IBV_MTU_1024;
	default_params.ib_devname = NULL;

	return create_context(hostname,iport,is_server,&default_params);
}

struct rdma_context* rdma_socket_p(const char* hostname,int iport, int is_server,struct init_params *params){
	if(params->port == 0) params->port = 18185;
	if(params->ib_port == 0) params->ib_port = 1;
	params->mtu = IBV_MTU_1024;
	params->rx_depth = 16000;
	return create_context(hostname,iport,is_server,params);
}
int counter = 0;
int rdma_post_send(struct rdma_context *ctx,struct rdma_task *task)
{

	struct ibv_sge list = {};
	list.addr = (uintptr_t) task->mem->buf;
	list.length = task->size;
	list.lkey = task->mem->mr->lkey;

/*
	if(counter != 0 && task->size != 21) {
		char filename[30];
		sprintf(filename,"/home/wz/log/send/%08d",counter-1);
		int newfile = creat(filename,0777);
	        if(newfile < 0) {
	                fprintf(stderr," create new file error\n");
	                exit(66);
	        }
		if(write(newfile,task->mem->buf,task->size) != task->size) {
	       		fprintf(stderr,"\n write to stdout error!\n");
	                exit(66);
	        }
	}
	if(task->size != 21) {
		++counter;
	}*/




	struct ibv_send_wr wr = {};
	wr.wr_id = task->id;
	wr.sg_list = &list;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;

	if(task->opcode != IBV_WR_SEND) {
		if(task->rkey == 0 && task->remote_addr == 0) {
			wr.wr.rdma.remote_addr = ctx->remote_info->addr;
			wr.wr.rdma.rkey = ctx->remote_info->rkey;
		}
		else {
			wr.wr.rdma.remote_addr = task->remote_addr;
			wr.wr.rdma.rkey = task->rkey;
		}
		wr.opcode = task->opcode;
		
	}
	//end
	struct ibv_send_wr *bad_wr;
	
	if(ibv_post_send(ctx->qp, &wr, &bad_wr)) {
		fprintf(stderr, "Couldn't post send\n");
			return -1;
	}

	return 0;
}

int rdma_query_cq(struct rdma_context *ctx,struct ibv_wc *wc,int maxCQE,int flag) {// send 1 , recv 2
	int use_event = 0;
	struct ibv_cq *cq;
	if(flag == 1)
		cq = ctx->send_cq;
	else if(flag == 2)
		cq = ctx->recv_cq;
	else return -1;
	if (use_event) {
			struct ibv_cq *ev_cq;
			void          *ev_ctx;

			if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
				fprintf(stderr, "Failed to get cq_event\n");
				return -1;
			}

			if (ev_cq != cq) {
				fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
				return -1;
			}

			if (ibv_req_notify_cq(cq, 0)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return -1;
			}
		}

		{
			int ne, i;

			do {
				ne = ibv_poll_cq(cq, maxCQE, wc);
				if (ne < 0) {
					fprintf(stderr, "poll CQ failed %d\n", ne);
					return -1;
				}

			} while (!use_event && ne < 1);

			for (i = 0; i < ne; ++i) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",ibv_wc_status_str(wc[i].status),
						wc[i].status, (int) wc[i].wr_id);
					return -1;
				}
			//fprintf(stdout,"%d.",wc[i].byte_len);
			}
			return ne;
		}
		
}

int rdma_close_ctx(struct rdma_context *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->send_cq)) {
		fprintf(stderr, "Couldn't destroy SEND CQ\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->recv_cq)) {
		fprintf(stderr, "Couldn't destroy RECV CQ\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}
	free(ctx);

	return 0;
}

int rdma_post_recv(struct rdma_context *ctx, struct rdma_task task) {
	struct ibv_sge list = {};
        list.addr = (uintptr_t)task.mem->buf;
        list.length = task.mem->size;
        list.lkey = task.mem->mr->lkey;

	struct ibv_recv_wr wr = {};
        wr.wr_id = task.id;
        wr.sg_list = &list;
        wr.num_sge = 1;

        struct ibv_recv_wr *bad_wr;

        return ibv_post_recv(ctx->qp, &wr, &bad_wr);
}


struct rdma_mem* rdma_reg_mr(struct rdma_context *ctx,void* buf,size_t size) {
	struct ibv_mr *mr = ibv_reg_mr(ctx->pd,buf,size,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ); 
	if(!mr) {
		fprintf(stderr, "Couldn't register MR\n");
		return NULL;
	}
	struct rdma_mem *rm = (struct rdma_mem*)calloc(1,sizeof *rm);
	rm->mr = mr;
	rm->buf = (char*)buf;
	rm->size = size;
	
	return rm;
}

int rdma_dereg_mr(struct rdma_mem *mem) {
	if (ibv_dereg_mr(mem->mr)) {
                fprintf(stderr, "Couldn't deregister RCV MR\n");
                return -1;
        }
	free(mem);
	return 0;
}

static const char *link_layer_str(uint8_t link_layer)
{
        switch (link_layer) {
        case IBV_LINK_LAYER_UNSPECIFIED:
        case IBV_LINK_LAYER_INFINIBAND:
                return "IB";
        case IBV_LINK_LAYER_ETHERNET:
                return "Ethernet";
        default:
                return "Unknown";
        }
}

const char* get_device(const char* name) {
	struct ibv_device **dev_list;
	dev_list = ibv_get_device_list(NULL);
	int i;
	struct ibv_port_attr port_attr;
	struct ibv_context* ctx;
        for(i = 0; dev_list[i];++i) {
                ctx = ibv_open_device(dev_list[i]);
                if(!ibv_query_port(ctx,1,&port_attr)) {
			if(!strcmp(name,link_layer_str(port_attr.link_layer))) {
				return ibv_get_device_name(dev_list[i]);
}
                }
        }

}


#endif
