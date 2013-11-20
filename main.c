#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "network.h"


// global variables
struct work_queue_item {
	int sock;
	char address[10];
	int port;
	struct work_queue_item *next;
};

int still_running = TRUE;
pthread_mutex_t work_mutex;
pthread_mutex_t write_mutex;
pthread_cond_t work_cond;
struct work_queue_item *head = NULL;
struct work_queue_item *tail = NULL;
int queue_count=0;
FILE *logfile;

void signal_handler(int sig) {
    still_running = FALSE;
}

void usage(const char *progname) {
    fprintf(stderr, "usage: %s [-p port] [-t numthreads]\n", progname);
    fprintf(stderr, "\tport number defaults to 3000 if not specified.\n");
    fprintf(stderr, "\tnumber of threads is 1 by default.\n");
    exit(0);
}

/* Removes the / from the beginning of a buffer if it is present */
void fix_buffer(char *buffer) {
	if ((char)buffer[0] == '/') {
		strcpy(buffer, buffer+1);
	}
}

/* Tests to see if a file exists, path indicated by a buffer
 * If it exists, updates size to indicate size of file
 * Returns 0 if successful and -1 otherwise
 */
int test_file(char *buffer, int *size) {
	struct stat st;
	if (stat(buffer, &st)==0) {
		*size = (int) st.st_size;
		return 0;
	}
	return -1;
}

/* Generates and returns a log entry string */
void gen_log(char *retbuffer, char *filename, struct work_queue_item *item, int fail, int size) {
	char logbuffer[1024];
	time_t now = time(NULL);

	char portstr[10];	
	sprintf((char*)&portstr, "%d ", item->port);
	char sizestr[10];
	sprintf((char*)&sizestr, "%d\n\0", size);
	
	strcpy(logbuffer, (char*)&(item->address));
	strcat(logbuffer, ":");
	strcat(logbuffer, (char*)portstr);
	strcat(logbuffer, strtok(ctime(&now),"\n"));
	strcat(logbuffer, " \"GET ");
	strcat(logbuffer, filename);

	if (fail)
		strcat(logbuffer, "\" 404 ");
	else
		strcat(logbuffer, "\" 200 ");

	strcat(logbuffer, (char*)sizestr);

	strcpy(retbuffer, logbuffer);
}

void *worker_thread() {
	printf("Worker initialized\n");
	while (still_running || queue_count > 0) {
		struct work_queue_item *tmp;

		// wait for item and remove from head
		pthread_mutex_lock(&work_mutex);
		while (queue_count==0) {
			printf("Worker going to sleep\n");
			pthread_cond_wait(&work_cond, &work_mutex);
		}
		printf("Worker received request\n");
		tmp = head;
		if (head->next == NULL) {
			head = NULL;
			tail = NULL;
		} else {
			head = head->next;
		}
		queue_count--;
		pthread_mutex_unlock(&work_mutex);

		// respond to request
		printf("Worker responding to request\n");
		char filename[1024];
		char sendbuffer[4096];
		int filesize=strlen(HTTP_404);
		int fd;
		int fail=1;

		if (getrequest(tmp->sock, (char*)&filename, 1024) == 0) {
			// request was successful - we have filename
			fix_buffer((char*)&filename);
			printf("Filename: %s\n",filename);

			if (test_file((char*)&filename,&filesize)==0) {
				// file exists - we have the size of the file
				fd = open((char*)&filename, O_RDONLY);
				printf("File exists\n");

				if (fd != -1) {
					// file successfully opened; send data!
					printf("Sending data...\n");
					fail=0;
					char *header = HTTP_200,filesize;
					senddata(tmp->sock, header, strlen(header));
					filesize += strlen(header);

					while (read(fd, (char*)&sendbuffer, 4096) != 0) {
						senddata(tmp->sock, (char*)&sendbuffer, 4096);
					}

					close(fd);	
					printf("Data transfer complete\n");
				}
			}
		}

		if (fail) {
			// failed to open/access file at some point along the way
			senddata(tmp->sock, HTTP_404, filesize);
		}

		// log request
		printf("Logging request\n");
		char logbuffer[1025];
		gen_log((char*)&logbuffer, (char*)&filename, tmp, fail, filesize);

		pthread_mutex_lock(&write_mutex);
		fputs(logbuffer, logfile);
		pthread_mutex_unlock(&write_mutex);

		free(tmp);
	}
	
	return NULL;
}

void runserver(int numthreads, unsigned short serverport) {
	// open/create log file
	logfile = fopen("weblog.txt","a");

	// initialize mutex/condition variables
    pthread_mutex_init(&work_mutex, NULL);
	pthread_mutex_init(&write_mutex, NULL);
	pthread_cond_init(&work_cond, NULL);

	// create worker threads
	pthread_t workers[numthreads];
	int i=0;
	for (; i < numthreads; i++) {
		printf("Creating worker %d\n",i);
		pthread_create(&workers[i], NULL, worker_thread, NULL);
	}
    
	// prepare socket
    int main_socket = prepare_server_socket(serverport);
    if (main_socket < 0) {
        exit(-1);
    }
    signal(SIGINT, signal_handler);

    struct sockaddr_in client_address;
    socklen_t addr_len;

    fprintf(stderr, "Server listening on port %d.  Going into request loop.\n", serverport);
    while (still_running) {
        struct pollfd pfd = {main_socket, POLLIN};
        int prv = poll(&pfd, 1, 10000);

        if (prv == 0) {
            continue;
        } else if (prv < 0) {
            PRINT_ERROR("poll");
            still_running = FALSE;
            continue;
        }
        
        addr_len = sizeof(client_address);
        memset(&client_address, 0, addr_len);

        int new_sock = accept(main_socket, (struct sockaddr *)&client_address, &addr_len);
        if (new_sock > 0) {
            
            time_t now = time(NULL);
            fprintf(stderr, "Got connection from %s:%d at %s\n", inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port), ctime(&now));

           /* You got a new connection.  Hand the connection off
            * to one of the threads in the pool to process the
            * request.
            *
            * Don't forget to close the socket (in the worker thread)
            * when you're done.
            */
			struct work_queue_item *new_item = (struct work_queue_item*) malloc(sizeof(struct work_queue_item));
			new_item->sock = new_sock;
			strcpy(new_item->address,inet_ntoa(client_address.sin_addr));
			new_item->port = ntohs(client_address.sin_port);
			new_item->next = NULL;

			pthread_mutex_lock(&work_mutex);

			if (tail==NULL) {
				tail = new_item;
				head = new_item;
			} else {
				// add new_item to tail
				tail->next = new_item;
			}
			queue_count++;
			pthread_cond_signal(&work_cond);

			pthread_mutex_unlock(&work_mutex);
			printf("Connection handed off to worker\n");

        }
    }
    fprintf(stderr, "Server shutting down.\n");

	free (head);
	free (tail);
    close(main_socket);
	fclose(logfile);
	for (i=0; i < numthreads-1; i++) {
		pthread_join(workers[i], NULL);
	}
	pthread_cond_destroy(&work_cond);
    pthread_mutex_destroy(&work_mutex);
}


int main(int argc, char **argv) {
    unsigned short port = 3000;
    int num_threads = 1;

    int c;
    while (-1 != (c = getopt(argc, argv, "hp:t:"))) {
        switch(c) {
            case 'p':
                port = atoi(optarg);
                if (port < 1024) {
                    usage(argv[0]);
                }
                break;

            case 't':
                num_threads = atoi(optarg);
                if (num_threads < 1) {
                    usage(argv[0]);
                }
                break;
            case 'h':
            default:
                usage(argv[0]);
                break;
        }
    }

    runserver(num_threads, port);
    
    fprintf(stderr, "Server done.\n");
    exit(0);
}
