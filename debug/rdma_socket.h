#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#ifndef RDMA_SOCKET_H
#define RDMA_SOCKET_H

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "pingpong.h"
#include "pingpong.c"

enum {
        PINGPONG_RECV_WRID = 1,
        PINGPONG_SEND_WRID = 2,
};
struct rdma_mem {
        struct ibv_mr           *mr;
        char                    *buf;
        size_t                     size;
};
struct rdma_task {
	struct rdma_mem		*mem;
	enum ibv_wr_opcode	opcode;
	size_t size;
	uint32_t rkey;
	uint64_t remote_addr;
	uint64_t	id;
};
struct rdma_context {
        struct ibv_context      *context;
        struct ibv_comp_channel *channel;
        struct ibv_pd           *pd;
        struct ibv_cq           *recv_cq;
	struct ibv_cq           *send_cq;
        struct ibv_qp           *qp;
	struct info_con  *remote_info;
        int               	id;
        int                      pending;
	int page_size;
	int ib_port;
        struct ibv_port_attr     portinfo;
	struct rdma_mem* rw;
	int fd;
	char* peer_host;
};

//for two qp to connect
struct info_con {
        uint32_t lid;
        uint32_t qpn;
        uint32_t psn;
	uint32_t rkey;		/* Remote key */
	uint64_t addr;		/* Buffer address */
        union ibv_gid gid;
};

//info for init_params 
struct init_params {
	int port;
	int ib_port;
	int gidx;
	int use_event;
	int rx_depth;
	int sockfd;
	size_t mem_size;
	enum ibv_mtu mtu;
	const char *ib_devname;
	int sr;//if include send receive
};


int rdma_post_recv(struct rdma_context *ctx, struct rdma_task *task,int nums);

const char* get_roce_name();
struct info_con *rdma_client_exch_dest(const char *servername, int port,const struct info_con *my_info); 

int rdma_connect_ctx(struct rdma_context *ctx, int port, int my_psn, enum ibv_mtu mtu, int sl, struct info_con *dest, int sgid_idx);

struct info_con *rdma_server_exch_dest(struct rdma_context *ctx, int ib_port, enum ibv_mtu mtu, int port, int sl, const struct info_con *my_info, int sgid_idx);

struct rdma_context* create_context(char* hostname,int iport, int is_server,struct init_params *params);

struct rdma_context* rdma_socket(const char* hostname,int iport, int is_server);

struct rdma_context* rdma_socket_p(const char* hostname,int iport, int is_server,struct init_params *params);

int rdma_post_send(struct rdma_context *ctx,struct rdma_task *task);

//int rdma_query_cq(struct rdma_context *ctx,int maxCQE,int *rcnt,int *scnt);

int rdma_close_ctx(struct rdma_context *ctx);

struct rdma_mem* rdma_reg_mr(struct rdma_context *ctx,void* buf,size_t size);
 
#endif
