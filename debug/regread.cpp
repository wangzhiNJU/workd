#include "rdma_session.cpp"
#include "rdma_portal.cpp"

//#define RW_SIZE 1048576
//#define RW_SIZE 1073741824

struct rdma_session* connect(const char* url,int size);
int collect(char* pool,int size);
void log(const char* str);
char* init_mem();
void move(char*,char*,long,int);
void reg_memory(struct rdma_session*,char*,char*,int,int,struct rdma_mem**);
void dereg_memory(struct rdma_mem** mems,int num);

size_t tg;

int main(int argc,char* argv[]) {

	int ss =strtol(argv[1],NULL,0);
	char* src = (char*)malloc(ss);// buffer

	tg = 1024*1024*1024;
	tg *= 2;

	// memory to be send
	long all = 1073741824;
	all *= 2;
	char* ptr = init_mem();

	int num = all/ss;
	int len = num*25;
	char data[20];

	const char* url = "192.168.1.88";
	struct rdma_session* session = connect(url,len);
	
	int fd = session->ctx->fd;


	struct timeval start,end;
	if (gettimeofday(&start, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	struct rdma_mem** mems = (struct rdma_mem**)malloc(sizeof (struct rdma_mem*)*num);
	reg_memory(session,ptr,session->get_rw_buf(),ss,len,mems);

	
	char head[20];
	sprintf(head,"%d",len);
	session->send_blocking(head,strlen(head)+1);
	log(head);
	session->read(data);
        if(strcmp(data,"done") != 0) {
        	log("got peer error, received:");
        }
	if (gettimeofday(&end, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	//closing
        dereg_memory(mems,num);	
	float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                        (end.tv_usec - start.tv_usec);
	printf("it costs : %.3f ms.\n",1.0*usec/1000);

	session->close();
	free_huge_pages(ptr);
	return 0;
}

void move(char* dest,char* src,long cur,int size) {
	size_t index = cur % tg;
	memcpy(dest,src+index,size);
}
 
struct rdma_session* connect(const char* url,int size) {
        struct rdma_session* rs;
        struct init_params params = {};
        params.mem_size = size;
	params.ib_devname = rs->device("IB");
	//params.ib_devname = rs->device("Ethernet");
        params.gidx = 0;
        srand48(getpid() * time(NULL));

        rs = new rdma_session(url,18185,&params);
        if(rs == NULL) {
                log(" null session");
        }
        return rs;
}

char* init_mem() {
	//char* ret = (char*)malloc(tg);
	char* ret = (char*)malloc_huge_pages(tg);
	if(NULL == ret) {
		log("malloc failed!\n");
	}

	char ch = 'a';
	size_t index = 0;
	while(index < tg) {
		memset(ret+index,ch,128);
		index += 128;
		if(ch == 'z') ch = 'a';
		else ++ch;
	}
	
	return ret;
}

void reg_memory(struct rdma_session* session,char* ptr,char* data,int bs,int len,struct rdma_mem** mems) {
	struct timeval start,end;
	gettimeofday(&start,NULL);
	size_t cur = 0;
	for(int i=0,j=0;cur<tg;cur+=bs,++j) {
		mems[j] = session->reg_mem(ptr+cur,bs);
		if(mems[j] == NULL) {
			log("rdma reg mr failed!\n");
			exit(1);
		}
		sprintf(data+i,"%016lu:%08x",mems[j]->mr->addr,mems[j]->mr->rkey);
		//fprintf(stdout,"%016lu:%08x	%d\n",mems[j]->mr->addr,mems[j]->mr->rkey,strlen(data+i));
		i += 25;
	}
	gettimeofday(&end,NULL);
	float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                        (end.tv_usec - start.tv_usec);
        printf("it costs : %.3f us.\n",1.0*usec);
}

void dereg_memory(struct rdma_mem** mems,int num) {
	for(int i=0;i<num;++i) {
		rdma_dereg_mr(mems[i]);
	}
}

void log(const char* str) {
	fprintf(stderr,"%s\n",str);
}
