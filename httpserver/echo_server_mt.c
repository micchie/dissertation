#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <netinet/in.h> 
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>


#define PORT 1234
#define Q_LEN 16
#define MAX_EVENTS 32
#define MAX_CLIENTS 10000
#define BUFFER_SIZE 1024
#define EPOLL_TIMEOUT 0

struct t_args{
    void* threadID;
    int epollfd;
    struct epoll_event ev;
    struct epoll_event * events;
    int listen_sock;
}t_args;


static void add_epoll_ctl(int epollfd, int socket, struct epoll_event ev){
    
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, socket, &ev) == -1){
        perror("adding listen_sock to epoll_ctl failed");
        exit(EXIT_FAILURE);
    }
}

//using void as a pointer lets you point to anything you like,
//and for some reason when threading you need to pass the arg struct as void
void *polling_thread(void *data){

    struct t_args *args = data;

    //unpack arguments
    int epollfd = args->epollfd;
    struct epoll_event ev = args->ev;
    struct epoll_event *events = args->events;
    int listen_sock = args->listen_sock;
    
    printf("polling thread started\n");

    for (;;){

        //returns the # of fd's ready for I/O
        int nfds = epoll_wait(epollfd, events, MAX_EVENTS, EPOLL_TIMEOUT);

        if (nfds == -1){
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }

        //loop through all the fd's to find new connections
        for (int n = 0; n < nfds; ++n){

            //if the listen socket is ready to read then we have a new connection
            int current_fd = events[n].data.fd;

            if (current_fd == listen_sock){

                struct sockaddr_in c_addr;//address of the client
                int c_addr_len = sizeof(c_addr);

                //assign socket to the new connection
                int conn_sock = accept(listen_sock, (struct sockaddr*)&c_addr, &c_addr_len);
                if (conn_sock == -1){
                    perror("cannot accept new connection");
                    exit(EXIT_FAILURE);
                }


                //set up ev for new socket
                ev.events = EPOLLIN;
                ev.data.fd = conn_sock;
                add_epoll_ctl(epollfd, conn_sock, ev);


            //if current_fd is not the listener we can do stuff
            }else {

                char buf[BUFFER_SIZE];

                bzero(buf, sizeof(buf));

                //read from socket, if we get an error then close all of the fd's
                int bytes_recv = read(events[n].data.fd, buf, sizeof(buf));

                //this part is  copied from michios code
                // removes the currentfd from the list of active fds
                if ( bytes_recv <= 0){
                    epoll_ctl(epollfd,EPOLL_CTL_DEL, current_fd, NULL);
                    close(current_fd);
                }else
                {
                    char *hello = "HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: 12\n\nHello world!";
                    //write(1, buf, sizeof(buf));
                    write(current_fd, hello, strlen(hello));
                }
                

                
            }
        }
    }


}


//set address and port for socket
static void set_sockaddr(struct sockaddr_in * addr){
    addr->sin_family = AF_INET;

    addr->sin_addr.s_addr = INADDR_ANY;//binds socket to all available interfaces
    //manual addr assignment: inet_aton('127.0.0.1', addr->sin_addr.s_addr);

    //htons convers byte order from host to network order 
    addr->sin_port = htons(PORT);
}

//set fd to non blocking, more portable than doing it in the socket definition
static int setnonblocking(int sockfd)
{
	if (fcntl(sockfd, F_SETFD, fcntl(sockfd, F_GETFD, 0) | O_NONBLOCK) ==-1) {
		return -1;
	}
	return 0;
}

int main(){
    
    struct epoll_event ev, events[MAX_EVENTS];
    int listen_sock, conn_sock, nfds, epollfd;
    int afds[MAX_CLIENTS]; //active file descriptors
    int n_afds = 0; //afds is like a stack so we need to keep track of our position

    //print various configuration settings
    printf("PORT: %d\n", PORT);
    printf("EPOLL_Q_LENGTH: %d\n", Q_LEN);
    printf("MAX_CLIENTS: %d\n", MAX_CLIENTS);
    printf("EPOLL_TIMEOUT: %d\n", EPOLL_TIMEOUT);


    //first we need to set up the addresses
    struct sockaddr_in s_addr;//addr we want to bind the socket to
    set_sockaddr(&s_addr);
    int s_addr_len = sizeof(s_addr);
    

    //set up listener socket
    listen_sock = socket(AF_INET, SOCK_STREAM, 0); 
    if (listen_sock == 0){ 
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    printf("Socket created, binding socket...\n");

    //set sock options that allow you to reuse addresses
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int))){
		perror("setsockopt");
		close(listen_sock);
	}


    //bind listener to addr and port 
    if ( bind(listen_sock, (struct sockaddr *) &s_addr, s_addr_len) < 0){
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    printf("socket bound, setting up listener...\n");


    //start listening for connections
    if (listen(listen_sock, Q_LEN) < 0){
        perror("listening failed");
        exit(EXIT_FAILURE);
    }
    printf("listener listening, we gucci\n");

    //create epoll instance
    epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (epollfd == -1) {
               perror("cant create epoll instance");
               exit(EXIT_FAILURE);
    }

    //populate ev with data so epoll will work
    ev.events = EPOLLIN; //type of event we are looking for
    ev.data.fd = listen_sock; 

    //add listen socket to interest list
    add_epoll_ctl(epollfd, listen_sock, ev);


    
    /*
    void* threadID;
    int epollfd;
    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];
    int listen_sock;
    */
   //populate args struct
    t_args.epollfd = epollfd;
    t_args.ev = ev;
    t_args.events = events;
    t_args.listen_sock = listen_sock;



    //now on to the actual polling
    pthread_t threads[4];//4 cores so 4 threads
    
    int rc;
    for (long i; i < 4; i++){
        printf("creating thread %ld\n", i);
        rc = pthread_create(&threads[i], NULL, polling_thread, &t_args);
        if (rc){
            printf("ERROR; return code from pthread_create() is %d\n", rc);
            exit(0);
        }
    }


    while (1){

    }



//branch for closing fd's
close_epfds:
    for (int i = 0; i < n_afds; i++) {
		close(afds[i]);
    }
	close(epollfd);

}
