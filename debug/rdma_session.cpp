#include "rdma_session.h"

rdma_session::rdma_session(struct rdma_context* ictx,struct init_params* params) {
	ctx = ictx;
	flag = 1;
	init(params);	
}

rdma_session::rdma_session(const char *hostname,int port,struct init_params* params) {
	ctx = rdma_socket_p(hostname,port,0,params);
	init(params);
}

void rdma_session::init(struct init_params* params) {
	recv_nums = 1024;
	maxCQE = recv_nums;
	gap = 32*1024;
	rdma_index = 0;
	rdma_gap = 2*1024*1024;
	sbuf_size = gap;
	rbuf_size = recv_nums*gap;

	reg_mem(gap);
	post_recv(16000);
	
	wc = new ibv_wc[maxCQE];
	send_wc = new ibv_wc[maxCQE];
	qbuf_size = 8*1024;
	qbuf = (char*)malloc(qbuf_size);
	door();
}

void rdma_session::door() {
	if(flag == 1) {
		start_rw();
		rdma_query_cq(ctx,wc,maxCQE,2);
		start_remote(wc[0].byte_len);	
	}
	else {
		rdma_query_cq(ctx,wc,maxCQE,2);
		start_remote(wc[0].byte_len);
		start_rw();
	}
}

void rdma_session::start_rw() {
	fprintf(stderr,"start rw\n");
	rwbuf_size = 1024*1024*1024;
	size_t slice = 2*1024*1024;
	int num = rwbuf_size/slice;
	slices = num;
	rwbuf = (char*)malloc_huge_pages(rwbuf_size);
	char* ptr = rwbuf;
	rw_mem = (struct rdma_mem**)malloc(sizeof (struct rdma_mem*)*num);
	int index = 0;
	for(int cur = 0;cur < rwbuf_size;cur+=slice) {
		rw_mem[index++] = reg_mem(ptr+cur,slice);
	}

	char data[num*25];
	for(int i=0,j=0;j<num;++j,i+=25) {
		sprintf(data+i,"%016lu:%08x",rw_mem[j]->mr->addr,rw_mem[j]->mr->rkey);
		fprintf(stderr,"%016lu:%08x %d\n",rw_mem[j]->mr->addr,rw_mem[j]->mr->rkey,j);
	}
	send(data,num*25);
	rdma_query_cq(ctx,send_wc,maxCQE,1);
	int r = 0;
}

void rdma_session::start_remote(uint32_t len) {
	fprintf(stderr,"start remote %d\n",len);
	++recv_index;
	char* ptr = rbuf;
	uint32_t num = len/25;
	char tmp[25];
	rkeys = (uint32_t*)malloc(sizeof (uint32_t) * num);
	raddrs = (uint64_t*)malloc(sizeof (uint64_t) * num);
	for(int i=0,cur=0;i<num;++i,cur+=25) {
		sscanf(ptr+cur,"%25s",tmp);
		sscanf(tmp,"%016lu:%08x",&raddrs[i],&rkeys[i]);
		fprintf(stderr,"%016lu:%08x %d\n",raddrs[i],rkeys[i],i);
	}
}

void rdma_session::reg_mem(size_t size) {
	recv_index = 0;
	recv_mem = (struct rdma_mem**)malloc(sizeof (struct rdma_mem*)*recv_nums);
	if(recv_mem == NULL) {
		fprintf(stderr,"Couldn't allocate recv memory region!\n");
		return;
	}
	int page_size = sysconf(_SC_PAGESIZE);
	//rbuf = (char*)memalign(page_size,size*recv_nums);//recv buffer size = 32*1024*100;
	rbuf = (char*)malloc_huge_pages(size*recv_nums);
	if(!rbuf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return;
	}

	for(int i=0;i<recv_nums;++i) {
		recv_mem[i] = rdma_reg_mr(ctx,rbuf+gap*i,gap);
		if(NULL==recv_mem[i]) {
			free(rbuf);
			fprintf(stderr, "Couldn't register MR in %d\n",i);
			return;
		}
	}
	//sbuf = (char*)memalign(page_size,size*recv_nums);//recv buffer size = 32*1024*100;
	sbuf = (char*)malloc_huge_pages(gap);
	if(!sbuf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return;
	}

	send_mem = rdma_reg_mr(ctx,sbuf,gap);
	if(!send_mem) {
		fprintf(stderr, "Couldn't register MR\n");
		rdma_dereg_mr(send_mem);
		return;
	}
}

struct rdma_mem* rdma_session::reg_mem(void* buf,size_t size) {
	return rdma_reg_mr(ctx,buf,size);
}

int rdma_session::send(struct rdma_task *task) {
	return rdma_post_send(ctx,task);
}

int rdma_session::rdma(struct rdma_mem* mem) {
	fprintf(stderr,"<index:%d,size:%ld> ",rdma_index,mem->size);
	struct rdma_task task = {};
	task.opcode = IBV_WR_RDMA_WRITE;
	task.size = mem->size;
	task.mem = mem;
	task.id = 233;
	task.remote_addr = raddrs[rdma_index];
	task.rkey = rkeys[rdma_index];
	rdma_index = (rdma_index+1)%slices;
	if(0 != send(&task))
		return -1;
	
	return 0;
}

int rdma_session::bulk(void* iptr,size_t size) {
	size_t num = size / rdma_gap + 1;
	size_t mod = size % rdma_gap;
	if(0 == mod)
		--num;
	struct rdma_mem** bulk_mems = (struct rdma_mem**)malloc(sizeof (struct rdma_mem*)*num);
	
	char* ptr = (char*)iptr;
	size_t cursor = 0,len = 0;
	int r , i = 0;
	while(cursor < size) {
		if(size - cursor >= rdma_gap) {
			len = rdma_gap;
		}
		else {
			len = size - cursor;
		}
		bulk_mems[i] = reg_mem(ptr+cursor,len);
		cursor += len;
		if(NULL == bulk_mems[i])
			return -1;
		++i;
	}

	fprintf(stderr,"\n CDE bulk %ld bytes in %d iters \n",size,num);
	for(i=0;i<num;++i) {
		r = rdma(bulk_mems[i]);
		if(0 != r)
			return -1;
	}
	logging("\n");

	int ack =0,tmp;
	while(ack != num) {
		tmp = rdma_query_cq(ctx,send_wc,maxCQE,1);
		if(tmp == -1)
			return -1;
		ack += tmp;
	}

	for(int i=0;i<num;++i) {
		r = rdma_dereg_mr(bulk_mems[i]);
		if(0 != r)
			return -1;
	}

	free(bulk_mems);
	return 0;
}

int rdma_session::latency(size_t size) {
	fprintf(stderr," CDELIVER %ld ",size);
	if(size >gap) {
		fprintf(stderr," bigger than gap ");
		return -1;
	}
	int ret = send(size);
	if(0 != ret)
		return ret;
	int ack = rdma_query_cq(ctx,send_wc,maxCQE,1);
	if(ack == -1) {
		fprintf(stderr," wrong for send cq\n");
		return -1;
	}
	return 0;
}

int rdma_session::send(size_t size) {
	if(size == 0) return 0;
	struct rdma_task task = {};
	task.opcode = IBV_WR_SEND;//TODO
	task.size = size;
	task.id = 222;
	task.mem = send_mem;
	int r = send(&task);
	return r;
}

int rdma_session::send(const char *buf,size_t size) {
	if(size > gap) {
		fprintf(stderr,"bigger than send buffer!");
	}
	memcpy(send_mem->buf,buf,size);
	struct rdma_task task = {};
	task.mem = send_mem;
	task.opcode = IBV_WR_SEND;//TODO
	task.size = size;
	task.id = 65;
	return send(&task);
}

int rdma_session::read(char* dest) {
	//fprintf(stderr," \n [ %d  ] \n",recv_index);
	int done = query(maxCQE);
	
	char* start = (char*)recv_mem[recv_index]->buf;
	int size = 0,full = 0,len=0;
	for(int i=0;i<done;++i) {
		if(wc[i].byte_len < gap) {
			len = full*gap+wc[i].byte_len;
			if(recv_index+full<recv_nums) {
				memcpy(dest+size,start,len);
				size += len;
			}
			else {
				int front = (recv_index+full)%recv_nums;
				int tlen = (recv_nums-recv_index)*gap;
				memcpy(dest+size,start,tlen);
				size += tlen;
				start = (char*)recv_mem[0]->buf;
				memcpy(dest+size,start,(len-tlen));
				size += (len-tlen);
			}
			recv_index = (recv_index+full+1)%recv_nums;
			start = (char*)recv_mem[recv_index]->buf;
			full = 0;
		}
		else {
			++full;//when this wc is full with the buffer
			continue;
		}
	}
	if(full != 0) {
		if(recv_index+full<recv_nums) {
			len = full *gap;
			memcpy(dest+size,start,len);
			size += len;
		}
		else {
			int front = (full+recv_index)%recv_nums;
			len = (recv_nums-recv_index)*gap;
			memcpy(dest+size,start,len);
			size += len;
			if(front != 0) {
				len = front*gap;
				start = (char*)recv_mem[0]->buf;
				memcpy(dest+size,start,len);
				size += len;
			}
		}
	}
	recv_index = (recv_index+full)%recv_nums;
	return size;
}

int rdma_session::query(int num) {
	int r = rdma_query_cq(ctx,wc,num,3);
	if(r > 0) {
		for(int i =0;i<r;++i) {
			if(wc[i].wr_id == 666)
				--posted_recv;
		}
	}
	return r;
}

int rdma_session::query_sr() {
	memset(qbuf,0,qbuf_size);
	int r = rdma_query_cq(ctx,wc,maxCQE,2);
	int cur=0,cnt = 0;
	if(r > 0) {
		for(int i =0;i<r;++i) {
			if(wc[i].wr_id == 666){
				sprintf(qbuf+cur,"%ld",wc[i].byte_len);
				fprintf(stderr," %ld.",wc[i].byte_len);
				cur += 8;
				++cnt;
			}
		}
	}
	
	return cnt;
}

int rdma_session::close() {
	if(recv_mem != NULL) {
		for(int i=0;i<recv_nums;++i)
			if (rdma_dereg_mr(recv_mem[i])) {
				fprintf(stderr, "Couldn't deregister RCV MR\n");
				return -1;
			}
	}
	
	if(send_mem != NULL) {
		if (rdma_dereg_mr(send_mem)) {
			fprintf(stderr, "Couldn't deregister SEND MR\n");
			return 1;
		}
	}
	if(rw_mem != NULL) {
		for(int i=0;i<slices;++i)
		{
			if (rdma_dereg_mr(rw_mem[i])) {
				fprintf(stderr, "Couldn't deregister RCV MR\n");
				return -1;
			}
		}
	}
	if(ctx->rw != NULL) {
		if (rdma_dereg_mr(ctx->rw)) {
        	        fprintf(stderr, "Couldn't deregister RW MR\n");
        	        return -1;
	        }
	}
	free(rbuf);
	free(sbuf);
	free(qbuf);
	free(rkeys);
	free(raddrs);
	free_huge_pages(rwbuf);
	return rdma_close_ctx(ctx);
}

int rdma_session::post_recv(int nums) {
	int i = 0,r=0;
	struct rdma_task task = {};
	while(i < 16000 && i < nums) {
		task.mem = recv_mem[i%recv_nums];
		task.id = 666;
		r = rdma_post_recv(ctx,task);
		if(r != 0 ) return r;
		++i;
	}
	posted_recv += i;
	return 0;
}

struct rdma_mem* rdma_session::set_mem(void* buf,size_t size) {
	return rdma_reg_mr(ctx,buf,size);
}

void rdma_session::logging(const char* str) {
	fprintf(stderr,str);
}
