#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <strings.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>
#include "chatServer.h"

enum bool {FALSE=0, TRUE=1};

static int end_server = FALSE;

void intHandler(int SIG_INT) {
    /* use a flag to end_server to break the main loop */
    end_server = TRUE;
}

int main (int argc, char *argv[])
{
    int on = 1;
    struct sockaddr_in addr;
    int port =0;

    if(argc != 2){
        fprintf(stderr, "Usage: ./chatServer <port>\n");
        return -1;
    }

    signal(SIGINT, intHandler);

    port = atoi(argv[1]);

    conn_pool_t* pool = malloc(sizeof(conn_pool_t));
    init_pool(pool);

    /* Create an AF_INET stream socket to receive incoming connections on      */
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0){
        perror("socket");
        return -1;
    }

    /* Set socket to be nonblocking. All of the sockets for      */
    /* the incoming connections will also be nonblocking since   */
    /* they will inherit that state from the listening socket.   */

    if(ioctl(sock, (int)FIONBIO, (char*)&on) == -1){
        perror("ioctl");
        return -1;
    }
    /* Bind the socket                                           */
    bzero(&addr, sizeof(struct sockaddr_in));
    addr.sin_family				= AF_INET;
    addr.sin_addr.s_addr	= htonl(INADDR_ANY);
    addr.sin_port					= htons(port);

    if( bind(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) < 0 ){
        perror("bind");
        return -1;
    }

    /* Set the listen back log  */
    if(listen(sock, 5) < 0){
        perror("listen");
        return -1;
    }

    /* Initialize fd_sets  	 */

    add_conn(sock, pool);
    /* Loop waiting for incoming connects, for incoming data or to write data, on any of the connected sockets.           */
    do{
        struct conn *conn;

        /* Copy the master fd_set over to the working fd_set.     */
        pool->ready_read_set  = pool->read_set;
        pool->ready_write_set = pool->write_set;

        /* Call select() 	*/

        printf("Waiting on select()...\nMaxFd %d\n", pool->maxfd);
        if(select(pool->maxfd + 1, &pool->ready_read_set,
                  &pool->ready_write_set, NULL, NULL) == -1){
            if(errno != EINTR){
                perror("select");
            }
            break;
        }

        /* One or more descriptors are readable or writable.      */
        /* Need to determine which ones they are.                 */

        /* check all descriptors, stop when checked all valid fds */
        for (conn = pool->conn_head; conn != NULL; conn = conn->next)
        {
            /* Each time a ready descriptor is found, one less has  */
            /* to be looked for.  This is being done so that we     */
            /* can stop looking at the working set once we have     */
            /* found all of the descriptors that were ready         */

            /* Check to see if this descriptor is ready for read   */
            if (FD_ISSET(conn->fd, &pool->ready_read_set)){

                /* A descriptor was found that was readable		   		*/
                /* if this is the listening socket, accept one      */
                /* incoming connection that is queued up on the     */
                /*  listening socket before we loop back and call   */
                /* select again. 	*/

                if(conn->fd == sock){
                    socklen_t slen = sizeof(struct sockaddr_in);
                    const int fd = accept(conn->fd, (struct sockaddr*) &addr, &slen);
                    if(fd == -1){
                        perror("accept");
                        end_server = TRUE;
                        break;
                    }
                    printf("New incoming connection on sd %d\n", fd);

                    if(ioctl(sock, (int)FIONBIO, (char*)&on) == -1){
                        perror("ioctl");
                        end_server = TRUE;
                        break;
                    }

                    add_conn(fd, pool);

                }else{
                    int eom = 0;	//end of message

                    printf("Descriptor %d is readable\n", conn->fd);

                    int msg_size = 100;
                    int msg_len = 0;
                    char * msg = malloc(sizeof(char)*(msg_size+1));
                    if(msg == NULL){
                        perror("malloc");
                        end_server = TRUE;
                        break;
                    }

                    /* If this is not the listening socket, an 					*/
                    /* existing connection must be readable							*/
                    /* Receive incoming data his socket            			*/
                    while(eom < 2){	//stop after receiving 2 terminators (\r\n)
                        const int rv = read(conn->fd, &msg[msg_len], 1);

                        if(rv < 0){
                            perror("read");
                            end_server = TRUE;
                            break;
                        }else if(rv == 0){
                            printf("Connection closed for sd %d\n", conn->fd);

                            /* If the connection has been closed by client 		*/
                            remove_conn(conn->fd, pool);
                            eom = -1;
                            //stop the loop since list has changed
                            break;
                        }else{
                            //if we have a message terminator
                            if(	(msg[msg_len] == '\r') ||
                                   (msg[msg_len] == '\n') ){
                                //increase count of terminators
                                eom++;
                            }
                            //incremase message length
                            msg_len += 1;

                            if(msg_len >= msg_size){
                                msg_size += 10;
                                msg = (char*) realloc(msg, sizeof(char)*msg_size);
                            }
                        }
                    }	//end while(receive message)

                    /* Data was received, add msg to all other  connections					  			  						*/

                    if(eom == 2){
                        printf("%d bytes received from sd %d\n", msg_len, conn->fd);

                        add_msg(conn->fd, msg, msg_len, pool);
                    }else if(eom == -1){	//conncetion closed
                        //stop the list loop
                        break;
                    }
                }
            } /* End of if (FD_ISSET()) */


            /* Check to see if this descriptor is ready for write  */

            if (FD_ISSET(conn->fd, &pool->ready_write_set)){
                /* try to write all msgs in queue to sd */
                write_to_client(conn->fd, pool);
            }
        } /* End of loop through selectable descriptors */

    } while (end_server == FALSE);

    /* If we are here, Control-C was typed,	clean up all open connections		 */

    conn_t *conn = pool->conn_head;
    while(conn != NULL){

        conn_t * next = conn->next;

        //close connection
        shutdown(conn->fd, SHUT_RDWR);
        close(conn->fd);

        free(conn);

        conn = next;
    }

    return 0;
}


int init_pool(conn_pool_t* pool) {
    //initialized all fields
    pool->maxfd = 0;
    pool->nready = 0;

    FD_ZERO(&pool->read_set);
    FD_ZERO(&pool->ready_read_set);
    FD_ZERO(&pool->write_set);
    FD_ZERO(&pool->ready_write_set);

    pool->conn_head = NULL;
    pool->nr_conns = 0;

    return 0;
}

int add_conn(int sd, conn_pool_t* pool) {
    /*
     * 1. allocate connection and init fields
     * 2. add connection to pool
     * */
    conn_t * con = (conn_t*) malloc(sizeof(conn_t));
    con->fd = sd;

    if(con->fd >= pool->maxfd){
        pool->maxfd = con->fd;
    }

    FD_SET(con->fd, &pool->read_set);
    //FD_SET(con->fd, &pool->write_set);

    con->prev = NULL;
    con->next = pool->conn_head;

    if(pool->conn_head == NULL){
        pool->conn_head = con;
    }else{
        pool->conn_head->prev = con;
    }
    pool->conn_head = con;

    con->write_msg_head = NULL;
    con->write_msg_tail = NULL;

    return 0;
}


int remove_conn(int sd, conn_pool_t* pool) {
    conn_t * conn = pool->conn_head;
    conn_t * prev = prev;

    while(conn != NULL){
        if(conn->fd == sd){
            break;
        }

        prev = conn;
        conn = conn->next;
    }

    if(conn){	//if found
        printf("removing connection with sd %d \n", conn->fd);

        //1. remove connection from pool
        if(prev){
            prev->next = conn->next;
        }else{
            pool->conn_head = conn->next;
        }

        // 3. remove from sets
        FD_CLR(conn->fd, &pool->read_set);
        FD_CLR(conn->fd, &pool->write_set);

        //4. update maxfd if needed
        if(pool->maxfd == conn->fd){
            //clear old max value
            pool->maxfd = 0;

            //find max fd
            conn_t * iter = pool->conn_head;
            while(iter != NULL){
                if(iter->fd > pool->maxfd){
                    pool->maxfd = iter->fd;
                }
                iter = iter->next;
            }
        }

        shutdown(conn->fd, SHUT_RDWR);
        close(conn->fd);

        //deallocate the message queue
        while(conn->write_msg_head){
            msg_t * next = conn->write_msg_head->next;

            free(conn->write_msg_head->message);
            free(conn->write_msg_head);

            conn->write_msg_head = next;
        }
        //2. deallocate connection
        free(conn);
    }

    return 0;
}

int add_msg(int sd,char* buffer,int len,conn_pool_t* pool) {

    /*
     * 1. add msg_t to write queue of all other connections
     * 2. set each fd to check if ready to write
     */
    conn_t * conn = pool->conn_head;
    while(conn != NULL){

        if(sd != conn->fd){

            msg_t * m = (msg_t*) malloc(sizeof(msg_t));
            if(m == NULL){
                perror("malloc");
                return -1;
            }
            m->message = buffer;
            m->size = len;

            //insert into doubly linked list
            if(conn->write_msg_head == NULL){
                conn->write_msg_head = m;

                //add client to write set on first message
                FD_SET(conn->fd, &pool->write_set);
            }
            m->prev = conn->write_msg_tail;
            m->next = NULL;
        }

        conn = conn->next;
    }

    return 0;
}

int write_to_client(int sd,conn_pool_t* pool) {

    /*
     * 1. write all msgs in queue
     * 2. deallocate each writen msg
     * 3. if all msgs were writen successfully, there is nothing else to write to this fd... */
    conn_t * conn = pool->conn_head;
    while(conn != NULL){
        if(conn->fd == sd){
            break;
        }
        conn = conn->next;
    }

    //if connection was found
    if(conn){

        while(conn->write_msg_head != NULL){
            msg_t * next = conn->write_msg_head->next;

            //send message
            write(conn->fd, conn->write_msg_head->message, conn->write_msg_head->size);

            //deallocate message
            free(conn->write_msg_head->message);
            free(conn->write_msg_head);

            //move to next message
            conn->write_msg_head = next;
        }
        //remove client from write set
        FD_CLR(conn->fd, &pool->write_set);
    }

    return 0;
}