#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "proxyserver.h"
#include "safequeue.h"

/*
 * Constants
 */
#define RESPONSE_BUFSIZE 10000

/*
 * Global configuration variables.
 * Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int num_listener;
int *listener_ports;
int num_workers;
char *fileserver_ipaddr;
int fileserver_port;
int max_queue_size;

// int client_fd: The file descriptor for the client socket.
// This is used to send data back to the client over the network.
void send_error_response(int client_fd, status_code_t err_code, char *err_msg)
{
    http_start_response(client_fd, err_code);
    http_send_header(client_fd, "Content-Type", "text/html");
    http_end_headers(client_fd);
    char *buf = malloc(strlen(err_msg) + 2);
    sprintf(buf, "%s\n", err_msg);
    http_send_string(client_fd, buf);
    return;
}

/*
 * forward the client request to the fileserver and
 * forward the fileserver response to the client
 */
void serve_request(int client_fd)
{
    /*This line creates a new socket for communicating with the file server.
    PF_INET specifies the use of the IPv4 Internet protocols. SOCK_STREAM indicates
     that the socket is a stream socket (suitable for TCP connections).*/
    // create a fileserver socket
    int fileserver_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fileserver_fd == -1)
    {
        fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
        exit(errno);
    }
    /* The address of the file server is set up using struct sockaddr_in. It includes
    the IP address (converted from a string to a network byte order using inet_addr())
     and the port number (converted to network byte order using htons()).
    */
    // create the full fileserver address
    struct sockaddr_in fileserver_address;
    fileserver_address.sin_addr.s_addr = inet_addr(fileserver_ipaddr);
    fileserver_address.sin_family = AF_INET;
    fileserver_address.sin_port = htons(fileserver_port);

    // This line attempts to establish a connection to the file server using the
    // previously created socket and address.
    int connection_status = connect(fileserver_fd, (struct sockaddr *)&fileserver_address,
                                    sizeof(fileserver_address));
    if (connection_status < 0)
    {
        // failed to connect to the fileserver
        printf("Failed to connect to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");
        return;
    }

    // successfully connected to the file server
    // This line attempts to establish a connection to the file server
    // using the previously created socket and address.
    char *buffer = (char *)malloc(RESPONSE_BUFSIZE * sizeof(char));

    // forward the client request to the fileserver
    int bytes_read = read(client_fd, buffer, RESPONSE_BUFSIZE);  // Reads data from the client socket.
    int ret = http_send_data(fileserver_fd, buffer, bytes_read); // Sends the read data to the file server.
    if (ret < 0)
    {
        printf("Failed to send request to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");
    }
    else
    {
        // forward the fileserver response to the client
        while (1)
        {
            // Receives data from the file server.
            int bytes_read = recv(fileserver_fd, buffer, RESPONSE_BUFSIZE - 1, 0);
            if (bytes_read <= 0) // fileserver_fd has been closed, break
                break;
            // Sends the received data to the client.
            ret = http_send_data(client_fd, buffer, bytes_read);
            if (ret < 0)
            { // write failed, client_fd has been closed
                break;
            }
        }
    }

    // close the connection to the fileserver
    shutdown(fileserver_fd, SHUT_WR);
    close(fileserver_fd);

    // Free resources and exit
    free(buffer);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
/*
Instead of using a global server_fd, I will use an array to store each threads server_fd.
*/

// int server_fd;   COMMENTING OUT old version of server_fd

#define MAX_LISTENERS 15 // adjust if expecting more than 100 threads.
int server_fds[MAX_LISTENERS];

void initialize_server_fds()
{
    for (int i = 0; i < MAX_LISTENERS; i++)
    {
        server_fds[i] = -1;
    }
}
//////////////////////////////////////////////////////////////////////////////////////////////////////
// creating a structure for our thread arguments
typedef struct
{
    int port;  // given in command line
    int index; // will be used to store the index of the thread in the server_fds array.
    PriorityQueue *pq;
} ThreadArgs;

/*
 * opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
//////////////////////////////////////////////////////////////////////////////////////////////////////////
// changing the function to take in a void pointer as an argument and return a void pointer.
// this is to allow the function to be used as a thread.
void *serve_forever(void *arg)
{

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    ThreadArgs *threadArgs = (ThreadArgs *)arg; // casting the void pointer to a ThreadArgs pointer.
    // next I'm getting the args into local variables.
    int port = threadArgs->port;
    int index = threadArgs->index;
    PriorityQueue *pq = threadArgs->pq;
    free(arg);
    // create a socket to listen
    int server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("Failed to create a new socket");
        exit(errno);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////////
    server_fds[index] = server_fd; // storing the server_fd in the server_fds array.

    // manipulate options for the socket
    // This call sets options on the socket. SO_REUSEADDR allows the server to
    // bind to a port which remains in TIME_WAIT state (useful for server restarts).
    // socket_option is set to 1 to enable this option.
    int socket_option = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) == -1)
    {
        perror("Failed to set socket options");
        exit(errno);
    }

    int proxy_port = port;
    // create the full address of this proxyserver
    // he port number is taken from listener_ports[0] and converted to network byte order using htons().
    struct sockaddr_in proxy_address;
    memset(&proxy_address, 0, sizeof(proxy_address));
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(proxy_port); // listening port

    // bind the socket to the address and port number specified in
    if (bind(server_fd, (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) == -1)
    {
        perror("Failed to bind on socket");
        exit(errno);
    }

    /*Marks the socket as a passive socket that will be used to accept incoming
    connection requests. 1024 is the maximum length of the queue of pending connections.
    */
    if (listen(server_fd, 1024) == -1)
    {
        perror("Failed to listen on socket");
        exit(errno);
    }

    printf("Listening on port %d...\n", proxy_port);

    struct sockaddr_in client_address;
    size_t client_address_length = sizeof(client_address);
    int client_fd;
    // The while loop uses accept to wait for and accept incoming connection requests.
    while (1)
    {
        // When a client connects, accept returns a new socket file descriptor for this specific connection.
        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_address,
                           (socklen_t *)&client_address_length);
        if (client_fd < 0)
        {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n",
               inet_ntoa(client_address.sin_addr),
               client_address.sin_port);

        ///////////////////////////////////////////////////////////////////////////////////////////////

        char *client_request_path = NULL;
        int priority = -1;
        int delay = -1;

        parse_client_request(client_fd, &client_request_path, &priority, &delay);

        // Now path, priority, and delay variables hold the extracted values
        // Remember to free path when you are done with it

        struct http_request *request = http_request_parse(client_fd);
        if (request == NULL)
        {
            // Handle parse error
            continue;
        }

        if (strcmp(request->path, GETJOBCMD) == 0)
        {
            // It's a GetJob request
            // Get the highest priority job from the priority queue
            QueueItem *job = get_work_nonblocking(pq);
            if (job == NULL)
            {
                // There are no jobs in the queue
                // Send an error response to the client
                send_error_response(client_fd, QUEUE_EMPTY, "Not Found");
            }
            else
            {
                ///NOT SURE ABOUT THIS FORMAT

                // There is a job in the queue
                // Send the job to the client
                http_start_response(client_fd, OK);
                http_send_header(client_fd, "Content-Type", "text/plain");
                http_end_headers(client_fd);
                http_send_string(client_fd, job->path);
            }
        }
        else
        {
            // It's not a GetJob request
            // Add the request to the priority queue
            add_work(pq, client_fd, request->path, priority, delay);
        }

        free(request->method);
        free(request->path);
        free(request);

        /**
         * commenting this out for now
         */
        // serve_request(client_fd);

        // close the connection to the client
        shutdown(client_fd, SHUT_WR);
        close(client_fd);
    }

    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
}

/*
 * Default settings for in the global configuration variables
 */
void default_settings()
{
    num_listener = 1;
    listener_ports = (int *)malloc(num_listener * sizeof(int));
    listener_ports[0] = 8000;

    num_workers = 1;

    fileserver_ipaddr = "127.0.0.1";
    fileserver_port = 3333;

    max_queue_size = 100;
}

void print_settings()
{
    printf("\t---- Setting ----\n");
    printf("\t%d listeners [", num_listener);
    for (int i = 0; i < num_listener; i++)
        printf(" %d", listener_ports[i]);
    printf(" ]\n");
    printf("\t%d workers\n", num_listener);
    printf("\tfileserver ipaddr %s port %d\n", fileserver_ipaddr, fileserver_port);
    printf("\tmax queue size  %d\n", max_queue_size);
    printf("\t  ----\t----\t\n");
}

void signal_callback_handler(int signum)
{
    printf("Caught signal %d: %s\n", signum, strsignal(signum));
    for (int i = 0; i < num_listener; i++)
    {
        ///////////////////////////////////////////////////////////////////////////////////
        // adjusting to use the global array
        if (server_fds[i] != -1 && close(server_fds[i]) < 0)
        {
            perror("Failed to close server_fd \n");
        }
    }
    free(listener_ports);
    exit(0);
}

char *USAGE =
    "Usage: ./proxyserver [-l 1 8000] [-n 1] [-i 127.0.0.1 -p 3333] [-q 100]\n";

void exit_with_usage()
{
    fprintf(stderr, "%s", USAGE);
    exit(EXIT_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////////////////////
void *worker_thread_function(void *arg)
{
    PriorityQueue *pq = (PriorityQueue *)arg;

    while (1)
    {
        // Attempt to get a job from the priority queue
        QueueItem *job = get_work(pq);

        // Check for delay in the request and sleep if necessary
        int delay = job->delay;
        if (delay > 0)
        {
            sleep(delay);
        }

        // Process the job
        serve_request(job->client_fd);

        // Cleanup
        free(job->path);
        free(job);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_callback_handler);

    /* Default settings */
    default_settings();

    int i;
    for (i = 1; i < argc; i++)
    {
        if (strcmp("-l", argv[i]) == 0)
        {
            num_listener = atoi(argv[++i]);
            free(listener_ports);
            listener_ports = (int *)malloc(num_listener * sizeof(int));
            for (int j = 0; j < num_listener; j++)
            {
                listener_ports[j] = atoi(argv[++i]);
            }
        }
        else if (strcmp("-w", argv[i]) == 0)
        {
            num_workers = atoi(argv[++i]);
        }
        else if (strcmp("-q", argv[i]) == 0)
        {
            max_queue_size = atoi(argv[++i]);
        }
        else if (strcmp("-i", argv[i]) == 0)
        {
            fileserver_ipaddr = argv[++i];
        }
        else if (strcmp("-p", argv[i]) == 0)
        {
            fileserver_port = atoi(argv[++i]);
        }
        else
        {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
            exit_with_usage();
        }
    }
    print_settings();

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Initialize the priority queue
    PriorityQueue *pq = create_queue(max_queue_size);

    initialize_server_fds(); // Initialize the server_fds array
    pthread_t threads[num_listener];
    for (int i = 0; i < num_listener; i++)
    {
        ThreadArgs *args = malloc(sizeof(ThreadArgs)); // Allocate memory for arguments
        args->port = listener_ports[i];
        args->index = i; // The index in the server_fds array
        args->pq = pq;
        if (pthread_create(&threads[i], NULL, serve_forever, args) != 0)
        {
            perror("Failed to create thread");
            // Handle error
        }
        
    }

    // Start worker threads
    pthread_t workers[num_workers];
    for (int i = 0; i < num_workers; i++)
    {
        if (pthread_create(&workers[i], NULL, worker_thread_function, (void *)pq) != 0)
        {
            perror("Failed to create worker thread");
            // Handle error
        }
        
    }

     // Join listener threads
    for (int i = 0; i < num_listener; i++) {
        pthread_join(threads[i], NULL);
    }

    // Join worker threads
    for (int i = 0; i < num_workers; i++) {
        pthread_join(workers[i], NULL);
    }

    exit(EXIT_SUCCESS);
}
