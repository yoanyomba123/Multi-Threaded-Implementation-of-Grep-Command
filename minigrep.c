/******************************************************************************
 * minigrep - search a directory for files containing a given string
 *            and print the line numbers and filenames where found.
 *
 *  Author: James A. Shackleford
 *
 * Compile with:
 *   $ gcc -o minigrep minigrep.c -pthread
 **********************************************/
//#define _BSD_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <semaphore.h>
#include <pthread.h>

#define MAX_THREAD_NUM 100
#define QUEUE_EMPTY 1
/***** CUSTOM TYPES **********************************/
struct queue {
    char path[PATH_MAX];
    struct queue *next;
};
typedef struct queue* queue_t;

typedef struct stopwatch {
    struct timeval start;
} stopwatch_t;

struct thread_args{
    int threadID;
    char* args;
};

struct workers_state {
	     int still_working;
	          pthread_mutex_t mutex;
		       pthread_cond_t signal;
		        };
 
// define thread worker state
static struct workers_state wstate = {
	      .still_working = QUEUE_EMPTY,
	           .mutex = PTHREAD_MUTEX_INITIALIZER,
		        .signal = PTHREAD_COND_INITIALIZER
				 };
/***************************/


/***** GLOBAL VARIABLES ******************************/
static unsigned int num_occurences = 0;
static unsigned int num_threaded_occurences = 0;
sem_t work_queue_lock;
queue_t work_queue;

/***************************/


/***** HELPER FUCTIONS: WORK QUEUE *******************/
#define QUEUE_INITIALIZER NULL
/* adds the contents path to the queue */
void enqueue (queue_t* head, char* path)
{
    queue_t next = *head;

    *head = malloc(sizeof(**head));
    (*head)->next = next;
    strcpy((*head)->path, path);
}
/* checks if our queue is empty */
int isempty(queue_t* head){
     queue_t cur = *head;
     queue_t* pp = head;
     int count = 0;

     while(cur){
	     if(cur->next == NULL && count == 0){
		return 0;
	     }
     }
     return 1;
}
/* removes the oldest item from the queue and populates it into path.
 * if the queue is empty, path is populated with NULL */
void dequeue(queue_t* head, char* path)
{
    queue_t  cur = *head;
    queue_t* pp = head;

    while (cur) {
        if (cur->next == NULL) {
            strcpy(path, cur->path);
            *pp = cur->next;
            free(cur);
            return;
        }

        pp = &cur->next;
        cur = cur->next;
    }
}
/***************************/


/***** HELPER FUCTIONS: CODE TIMING ******************/
void stopwatch_start (stopwatch_t* sw)
{
    gettimeofday(&sw->start, NULL);
}

float stopwatch_report (stopwatch_t* sw)
{
    struct timeval stop;

    gettimeofday(&stop, NULL);
    return (float)(stop.tv_sec - sw->start.tv_sec + (stop.tv_usec - sw->start.tv_usec)/1000000.0);
}
/***************************/


/***** HELPER FUCTIONS: PRINT USAGE ******************/
void print_usage (char* prog)
{
    printf("Usage: %s mode path string \n\n", prog);
    printf("    mode    -   either -S for single thread or -P for pthreads\n");
    printf("    path    -   recursively scan all files in this path and report\n");
    printf("                   all occurances of string\n");
    printf("    string  -   scan files for this string\n\n");
}
/***************************/




/******************************************************************************
 *********************  M I N I   G R E P   S T A R T *************************
 ******************************************************************************/

/* Decend into the directory located at "current_path" and add all
 * the files and/or directories it contains to the work_queue */
unsigned int handle_directory (queue_t* work_queue, char* current_path)
{
    DIR *ptr_dir = NULL;
    struct dirent *ptr_result;
    char new_path[PATH_MAX];

    ptr_dir = opendir(current_path);
    if (!ptr_dir)
        return -1;

    /* scan through all files within the directory */
    while (1) {
        /* obtain a pointer to the current directory entry and store
         * it in ptr_entry.  if ptr_result is NULL, we have
         * cycled through all items in the directory */
        ptr_result = readdir(ptr_dir);

        if (ptr_result == NULL)
            break;

        /* Ignore "." (this directory) and ".." (parent directory) */
        if (!strcmp(ptr_result->d_name, ".") || !strcmp(ptr_result->d_name, ".."))
            continue;

        /* add the file or directory to the work queue */
        strcpy(new_path, current_path);
        strcat(new_path, "/");
        strcat(new_path, ptr_result->d_name);
	enqueue(work_queue, new_path);
    }
    closedir(ptr_dir);

    return 0;
}



/* Decend into the directory located at "current_path" and add all
 * the files and/or directories it contains to the work_queue - Prevent Threads From simultaneously 
 * removing or adding to the directory 
 */
unsigned int handle_threaded_directory (queue_t* work_queue, char* current_path)
{
    DIR *ptr_dir = NULL;
    struct dirent *ptr_result;
    char new_path[PATH_MAX];

    ptr_dir = opendir(current_path);
    if (!ptr_dir)
        return -1;

    /* scan through all files within the directory */
    while (1) {
        /* obtain a pointer to the current directory entry and store
         * it in ptr_entry.  if ptr_result is NULL, we have
         * cycled through all items in the directory */
        ptr_result = readdir(ptr_dir);

        if (ptr_result == NULL)
            break;

        /* Ignore "." (this directory) and ".." (parent directory) */
        if (!strcmp(ptr_result->d_name, ".") || !strcmp(ptr_result->d_name, ".."))
            continue;

        /* add the file or directory to the work queue */
        strcpy(new_path, current_path);
        strcat(new_path, "/");
        strcat(new_path, ptr_result->d_name);

	// decrement the semaphore	
	sem_wait(&work_queue_lock); // wait on semaphore
	enqueue(work_queue, new_path);
	// increment the semaphore
        sem_post(&work_queue_lock); // unlock semaphore
    }

    closedir(ptr_dir);
    return 0;
}

/* Search the file located at "current_path" for "string" line-by-line.
 * If we find a line that contains the string, we print the name of
 * the file, the line number, and the line itself. - threaded implementation */
unsigned int handle_threaded_file (char* current_path, char* string)
{
    FILE *fp;
    char *offset;
    size_t len = 0;
    char* line = NULL;
    unsigned int line_number = 0;

    // printf("READING %s \n", current_path);
    fp = fopen(current_path, "r");
    if (fp == NULL)
        return -1;

    while (getline(&line, &len, fp) != -1 ) {
        line_number++;

        /* get offset of substring "string" within the string "line" */
        offset = strstr(line, string);
        if (offset != NULL) {
            printf("%s:%u: %s", current_path, line_number, line);
            num_threaded_occurences++;
        }
    }
    fclose(fp);
    free(line);


   // check if queue is empty and change condition variable
   if(work_queue == NULL){
	wstate.still_working = 0;
	pthread_cond_broadcast(&wstate.signal);
   }
    return 0;
}


/* Search the file located at "current_path" for "string" line-by-line.
 * If we find a line that contains the string, we print the name of
 * the file, the line number, and the line itself. */
unsigned int handle_file (char* current_path, char* string)
{
    FILE *fp;
    char *offset;
    size_t len = 0;
    char* line = NULL;
    unsigned int line_number = 0;

    fp = fopen(current_path, "r");
    if (fp == NULL)
        return -1;

    while (getline(&line, &len, fp) != -1 ) {
        line_number++;

        /* get offset of substring "string" within the string "line" */
        offset = strstr(line, string);
        if (offset != NULL) {
            printf("%s:%u: %s", current_path, line_number, line);
            num_occurences++;
        }
    }
    fclose(fp);
    
    free(line);

    return 0;
}


/* Given a starting path, minigrep_simple using a single thread
 * to recursively search all files and directories within path
 * for the specified string */
void minigrep_simple (char* path, char* string)
{
    int ret;
    struct stat file_stats;
    char current_path[PATH_MAX];
    queue_t work_queue = QUEUE_INITIALIZER;

    /* the path specified on the command line is the first work item */
    enqueue(&work_queue, path);

    /* While there is work in the queue, process it. */
    while(work_queue != NULL){

        /* get the next item from the work queue */
        dequeue(&work_queue, current_path);
        /* and retrieve its file type information */
        lstat(current_path, &file_stats);

        /* if work item is a file, scan it for our string
         * if work item is a directory, add its contents to the work queue */
        if (S_ISDIR(file_stats.st_mode)) {
            /* work item is a directory; descend into it and post work to the queue */
            ret = handle_directory(&work_queue, current_path);
	    if (ret < 0) {
                fprintf(stderr, "warning -- unable to decend into %s\n", current_path);
                continue;
            }
        }
        else if (S_ISREG(file_stats.st_mode)) {
            /* work item is a file; scan it for our string */
            ret = handle_file(current_path, string);
            if (ret < 0) {
                fprintf(stderr, "warning -- unable to open %s\n", current_path);
                continue;
            }
        }
        else if (S_ISLNK(file_stats.st_mode)) {
            /* work item is a symbolic link -- do nothing */
        }
        else {
            printf("warning -- skipping file of unknown type %s\n", current_path);
        }
    }

    printf("\n\nFound %u instance(s) of string \"%s\".\n", num_occurences, string);
}



/* Thread Worker function utilized to parse through files and directory 
 * and access the worker queue
 */
void *thread_process(void *argument){
    struct thread_args *current_thread_arg = (struct thread_args*)argument;
    int status, threadID,ret; 
    struct stat file_stats;
    char* string;
    char current_path[PATH_MAX];
    threadID = current_thread_arg->threadID;
    string = current_thread_arg->args;

    while(work_queue != NULL){
	// decrement the semaphore
        sem_wait(&work_queue_lock);
	// remove item from queue
        dequeue(&work_queue, current_path);
        
	// increment semaphore once again when element taken from queue
        sem_post(&work_queue_lock);
         
        /* and retrieve its file type information */
        lstat(current_path, &file_stats);

        /* if work item is a file, scan it for our string
         * if work item is a directory, add its contents to the work queue */
        if (S_ISDIR(file_stats.st_mode)) {
            /* work item is a directory; descend into it and post work to the queue */
            ret = handle_threaded_directory(&work_queue, current_path);
            if (ret < 0) {
                fprintf(stderr, "warning -- unable to decend into %s\n", current_path);
                continue;
            }
        }
        else if (S_ISREG(file_stats.st_mode)) {
            /* work item is a file; scan it for our string */
            ret = handle_threaded_file(current_path, string);
            if (ret < 0) {
                fprintf(stderr, "warning -- unable to open %s\n", current_path);
                continue;
            }
        }
        else if (S_ISLNK(file_stats.st_mode)) {
            /* work item is a symbolic link -- do nothing */
        }
        else {
            printf("warning -- skipping file of unknown type %s\n", current_path);
        }
    }


}


/* Given a starting path, minigrep_pthreads uses multiple threads
 * to recursively search all files and directories within path
 * for the specified string */
void minigrep_pthreads(char* path, char* string)
{
    /* YOU DESIGN THE CONTENTS OF THIS FUNCTION */
   
    // initialize mutex
    sem_init(&work_queue_lock,0,1);
	
    int i, ret;
    struct stat file_stats;
    char current_path[PATH_MAX];
    struct thread_args* thread_arg;

    work_queue = QUEUE_INITIALIZER;
    int num_threads = MAX_THREAD_NUM;

    pthread_t mainthread;
    pthread_t worker_threads[num_threads];
    // return thread ID of calling thread
    mainthread = pthread_self();
    // add current path to queue
    enqueue(&work_queue, path);
    for(i = 0; i < MAX_THREAD_NUM; i++){
        thread_arg = (struct thread_args*)malloc(sizeof(struct thread_args));
        thread_arg->threadID = i;
        thread_arg->args = string;
        if(pthread_create(&worker_threads[i], NULL, thread_process, (void *)thread_arg)!=0){
        }
	// thread is detached w/ system resources to be relinquished by kernel
        pthread_detach(worker_threads[i]);
    }

   // implement locking mechanism in the main function as well
   pthread_mutex_lock(&wstate.mutex);
   // define the critical section
   // wait until the still working conditional is equal to 0
   while(wstate.still_working != 0){
       pthread_cond_wait(&wstate.signal, &wstate.mutex);                                                          
   }
   pthread_mutex_unlock(&wstate.mutex); 
   printf("\n\nFound %u instance(s) of string \"%s\".\n", num_threaded_occurences, string);
}


int main(int argc, char** argv)
{
    stopwatch_t T;

    if(argc < 4){
        print_usage (argv[0]);
        return EXIT_FAILURE;
    }

    if (!strcmp(argv[1], "-S")) {
        /* Perform a serial search of the file system */
        stopwatch_start(&T);
        minigrep_simple(argv[2], argv[3]);
        printf("Single Thread Execution Time: %f\n", stopwatch_report(&T));
    }
    else if (!strcmp(argv[1], "-P")) {
        /* Perform a multi-threaded search of the file system */
        stopwatch_start(&T);
        minigrep_pthreads(argv[2], argv[3]);
        printf("Multi Threads Execution Time: %f\n", stopwatch_report(&T));
    }
    else {
        printf("error -- invalide mode specified\n\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
