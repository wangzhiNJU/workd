#include "rdma_session.cpp"
#include "rdma_portal.cpp"

//#define RW_SIZE 1048576
//#define RW_SIZE 1073741824

struct rdma_session* connect(const char* url,int size);
int collect(char* pool,int size);
void log(const char* str);
char* init_mem();
void move(char*,char*,long,int);

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

	const char* url = "192.168.1.3";
	struct rdma_session* session = connect(url,ss);
	
	char data[20];
	int fd = session->ctx->fd;

	log("pre work done!\n");

	struct timeval start,end;
	if (gettimeofday(&start, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	
	long cur = 0;
	while(cur < all) {
		move(session->get_rw_buf(),ptr,cur,ss);
		sprintf(data,"%d",ss);
		//write(fd,data,strlen(data)+1);
		//read(fd,data,10);
		session->send_blocking(data,strlen(data)+1);
		int r = session->read(data);
		if(strcmp(data,"done") != 0) {
			log("got peer error, received:");
			log(data);
			break;
		}
		if(session->get_posted_recv() < 1000) {
			session->post_recv(15000);
		}

		cur += ss;
	}
	log("im out of the while\n");
	//write(fd,"0",2);
	session->send_blocking("0",2);	

	if (gettimeofday(&end, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                        (end.tv_usec - start.tv_usec);
	printf("it costs : %.3f ms.\n",1.0*usec/1000);

	session->close();
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
	params.ib_devname = rs->device("Ethernet");
        params.gidx = 0;
        srand48(getpid() * time(NULL));

        rs = new rdma_session(url,18185,&params);
        if(rs == NULL) {
                log(" null session");
        }
        return rs;
}

char* init_mem() {
	char* ret = (char*)malloc(tg);
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

void log(const char* str) {
	fprintf(stderr,"%s",str);
}
