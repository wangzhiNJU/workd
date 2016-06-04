#include "rdma_session.cpp"
#include "rdma_portal.cpp"

void debug(const char* msg) {
	fprintf(stderr,"%s\n",msg);
}

int client(char *servername,int size,int iter) {

	rdma_session *rs;
	struct init_params params = {};
	params.gidx = 0;
	params.ib_devname = rs->device("Ethernet");
	//params.ib_devname = rs->device("IB");
	debug(params.ib_devname);
	params.sr = 1;
	params.rx_depth = iter;
	params.mem_size = size;
	srand48(getpid() * time(NULL));
	
	rs = new rdma_session(servername,18185,&params);
	struct rdma_task task = {};
	task.mem = rs->send_mem;
	task.size = size;
	task.opcode = IBV_WR_SEND;

	size_t bytes = size*iter;
	char* ts = (char*)malloc(bytes);
	size_t index = 0;
	char ch = 'A';
	while(index < bytes) {
		memset(ts+index,ch,4);
		index += 4;
		++ch;
		if(ch == 'Z'+1) ch = 'A';
	}
	
	size_t cur = 0;
	char* dest = (char*)rs->send_mem->buf;
	memcpy(dest,ts+cur,size);
	cur+=size;
	
	
	int r = rs->send(&task);
	if(r != 0) {
		return 1;
	}

	struct timeval start,end;
	if (gettimeofday(&start, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	
	int scnt = 0;
	uint64_t tmp;
        //while(scnt < iter) {
        while(cur<bytes){
        	r = rdma_query_cq(rs->ctx,rs->send_wc,1,1);
                if(r == -1) {
                	fprintf(stderr,"fail to query!\n");
                	return 1;
                }
	memcpy(dest,ts+cur,size);
	cur+=size;
		scnt += r;
               	rs->send(&task);
      	}
        if (gettimeofday(&end, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	{
                float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                        (end.tv_usec - start.tv_usec);
                long long bytes = (long long) size * iter;
                printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
                       bytes, usec / 1000000., bytes * 8. / usec);
                printf("%d iters in %.2f seconds = %.2f usec/iter\n",
                       iter, usec / 1000000., usec / iter);

	}
	printf("\ndd\n");
        r = rs->close();
        fprintf(stdout,"close_ctx:%d\n",r);
}

int server(rdma_portal *rp,int size,int iter) {

	rdma_session *rs;
	struct init_params params = {};
	params.ib_devname = rs->device("Ethernet");
	//params.ib_devname = rs->device("IB");
	debug(params.ib_devname);
	params.sr = 1;
	params.gidx = 0;
	params.rx_depth = iter;
	params.mem_size = size;
	srand48(getpid() * time(NULL));

	fprintf(stdout,"sockfd : %d\n",rp->get_sockfd());
	rs = rp->accept(18185,&params);	
	fprintf(stdout,"sockfd : %d  host : %s\n",rp->get_sockfd(),rs->get_peer_host());
	size_t bytes = size*iter;
	char* ts = (char*)malloc(bytes);
	memset(ts,'a',bytes);
	int cur = 0;

	struct timeval start,end;
	if (gettimeofday(&start, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	int rcnt=0,r,ii=0;
        while(cur<bytes) {
		rcnt=rs->read(ts+cur);
		cur+=rcnt;
		if(ii==0) {
			ii=1;
			if (gettimeofday(&start, NULL)) {
                		perror("gettimeofday");
                		return 1;
        		}		
		}
        }

        if (gettimeofday(&end, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	fprintf(stdout,"\nbingo! recved : %d \n",cur);

	char* s = ts;
	cur = 0;
	char ch = 'A';
	while(cur < bytes) {
		if(s[0] == ch && s[0] == s[1] && s[0] == s[2] && s[0] == s[3]) {
			//printf("%c%c%c%c",s[0],s[1],s[2],s[3]);
			s += 4;
			++ch;
			cur += 4;
			if(ch == 'Z'+1) ch = 'A';
		}
		else {
			char chs[16];
			memcpy(chs,s,15);
			chs[15] = '\0';
			fprintf(stdout," wrong in %d. %s [%c]\n",cur,chs,ch);
			return 0;
		}
	}


	{
                float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                        (end.tv_usec - start.tv_usec);
                long long bytes = (long long) size*iter;

                printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
                       bytes, usec / 1000000., bytes * 8. / usec);
                printf("%d iters in %.2f seconds = %.2f usec/iter\n",
                       iter, usec / 1000000., usec / iter);
        }
        //ibv_ack_cq_events(ctx->cq, num_cq_events);
        r = rs->close();
        fprintf(stdout,"close_ctx:%d\n",r);
}

int main(int argc,char* argv[])
{
	rdma_portal * rp = new rdma_portal(1);
	int size = strtol(argv[1],NULL,0);
	int iter = strtol(argv[2],NULL,0);
	if(argc > 3) {
		client(argv[3],size,iter);
	}
	else {
		server(rp,size,iter);
	}
}

