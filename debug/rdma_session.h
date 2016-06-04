#include "rdma_socket.c"
class rdma_session 
{
private:
	uint64_t id;
	int maxCQE;
	

	int recv_index;//recv buffer
	int recv_nums;
	int posted_recv;
	int gap;

public:
	//temp
	struct rdma_context *ctx;
	struct rdma_mem **recv_mem;
	struct rdma_mem *send_mem;	
	struct rdma_mem **rw_mem;
	struct ibv_wc *wc;
	struct ibv_wc *send_wc;
	uint32_t *rkeys;
	uint64_t *raddrs;
	char* rbuf;
	char* sbuf;
	char* qbuf;

	char* rwbuf;
	int rwbuf_size;
	int rdma_index;
	int slices;
	size_t rdma_gap;

	int rbuf_size;
	int qbuf_size;
	int sbuf_size;
	int flag;

	rdma_session(struct rdma_context *ictx,struct init_params* params);
	rdma_session(const char* hostname,int port,struct init_params* params);
	void init(struct init_params* p);
	void reg_mem(size_t size);
	struct rdma_mem* reg_mem(void* buf,size_t size);
	int send(struct rdma_task *task);
	int send(const char *buf,size_t size);
	int send(size_t size);
	int latency(size_t size);
	int bulk(void*,size_t);
	int rdma(struct rdma_mem*);
	int read(char* dest);
	int query(int);
	int query_sr();
	int post_recv(int nums);
	int post_bufs();
	int close();
	void set_maxCQE(int cqe) 
	{
		maxCQE = cqe;
	}
	struct rdma_mem* get_send_mem() { return send_mem;}
	struct rdma_mem*  set_mem(void* buf,size_t size);
	struct ibv_wc* get_wc() { return wc;}
	char* get_rw_buf() {return ctx->rw->buf;}
	const char* device(const char* name) { return get_device(name);}
	int get_posted_recv() { return posted_recv;}
	char* get_peer_host() { return ctx->peer_host;}
	void start_rw();
	void start_remote(uint32_t);
	void door();
	void logging(const char*);
};
