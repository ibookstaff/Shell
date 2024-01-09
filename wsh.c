#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h> 
#include <sys/stat.h>
#include <termios.h>
#include <stdbool.h>
#include <errno.h>

// use these to return control to shell
pid_t shell_pgid;
int shell_terminal;
struct termios shell_tmodes;
int shell_is_interactive;

typedef struct process
{
  struct process *next;    // next process used when piping   
  char *argv[254];         // holds the instructions to execute
  pid_t pid;                // pid of the process executed
  char completed;           // whether or not job has been completed
  bool stopped;             // reccords if job is stopped
  int status;               // execution status whether or not it has been terminated  
} process;

typedef struct JOB
{
  struct JOB *next;         // holds the next job, so it acts as a linked list
  char *name[254];           // also holds the instructions   
  process *first_process;     // points the the process associated with the job
  pid_t pgid;                 // group id for the job
  struct termios tmodes;      // for shell control
  int stdin, stdout, stderr;  // which files to output/input to, used mostly for piping
  bool background;            // tracks if job is in background
  bool isPiped;               // tracks if the job has multiple proccesses
  bool jobComplete;           // tracks if job is completed
  bool fromBackground;        // need to know if the job is coming from the background
  int id;                     // used to give a job an ID for when it is put in background
} JOB;

/* The active jobs are linked into a list.  This is its head.   */
JOB *firstJob = NULL;   //linked listj to hold all our jobs
int idCounter = 1;       // used to assign id when job goes to background
struct termios shell_tmodes;    // for shell control
struct termios *ptr_shell_tmodes;    //for shell control



/*Used to change directories for built in command*/
void handle_cd(char *line[]){
    if(chdir(line[1]) != 0){
      printf("Directory: %s does not exist.\n", line[1]);
    }
    
}

/*Puts the job back into the background*/
void put_job_in_background (JOB *j, int cont){
  if (kill (j->pgid, SIGCONT) < 0){
    perror ("kill (SIGCONT)");
  }
}

/*This checks for signals and the status of a job when waiting for its completeion
  inspired by gnu.org*/
int mark_process_status (pid_t pid, int status){
  JOB *j;
  process *p;

  if (pid > 0){
      /* Update the record for the process.  */
      for (j = firstJob; j; j = j->next){
        for (p = j->first_process; p; p = p->next){
          if (p->pid == pid){
              p->status = status;
              // check if a signal is sent
              if (WIFSTOPPED (status)){
                p->stopped = 1;
                if(j->id < 1){
                  j->id = idCounter;
                  idCounter++;
                }
              }
              else{
                //  job has been completed
                  p->completed = true;                   
                }           
              return 0;
             }
        }
      }
      return -1;
    }
  else if (pid == 0)
    return -1;
  else {
    return -1;
  }
}

/*Used to continue waiting until the job is stopped
  allows us to check all piped processes*/
int job_is_stopped (JOB *j){
  process *p;

  for (p = j->first_process; p; p = p->next){
    if (!p->completed && !p->stopped){
      return 0;
    }
  }
  return 1;
}

/*Similar to job_is_stopped, checks if the job was stopped or completed
  allows us to check all piped processes*/
int job_is_completed (JOB *j){
  process *p;

  for (p = j->first_process; p; p = p->next){
    if (!p->completed){
      return 0;
    }
  }
  return 1;
}

/*Waits for the given job to be completed or signaled with Ctrl C/Z */
void wait_for_job (JOB *j){
  int status;
  pid_t pid;
  // if the job originated from the background, we need to wait for the 
  // specific pgid or we will end up waiting for all bg jobs
  if(j->fromBackground){
    do{
      waitpid (j->pgid, &status, WUNTRACED);
    }while (!mark_process_status (j->pgid, status)
         && !job_is_stopped (j)
         && !job_is_completed (j));

  }
  // if not from background, we have nothing else running and therefore wait for any job
  else{
    do{
      pid = waitpid (WAIT_ANY, &status, WUNTRACED);
    }while (!mark_process_status (pid, status)
         && !job_is_stopped (j)
         && !job_is_completed (j));

  }

}

/*Puts job the was executed into the foreground
  inpired by gnu.org*/
void put_job_in_foreground (JOB *j, int cont){
  // give up the shell as the foreground process
  tcsetpgrp (shell_terminal, j->pgid);
  if (cont){
    //restore as needed
      tcsetattr (shell_terminal, TCSADRAIN, &j->tmodes);
      if (kill (- j->pgid, SIGCONT) < 0)
        perror ("kill (SIGCONT)");
    }

  // wait for the job to complete or be signaled
  wait_for_job (j);

  // put the shell back into the foreground
  tcsetpgrp (shell_terminal, shell_pgid);

  // restore
  tcgetattr (shell_terminal, &j->tmodes);
  tcsetattr (shell_terminal, TCSADRAIN, &shell_tmodes);
}

/*After launching and setting up the job, we now want to lauunch the instructions execution
   inspired by gnu.org*/
void launch_process (process *p, pid_t pgid, int in, int out, int err, int foreground){
    pid_t pid;
    // add the new proccess into its own process group
    pid = getpid ();
    if (pgid == 0) pgid = pid;
    setpgid (pid, pgid);

    // if it is a foreground process, then we need to give the new process control      
    if (foreground)
      tcsetpgrp (shell_terminal, pgid);
    
    // return signals back to their default meanings for this process
    signal (SIGINT, SIG_DFL);
    signal (SIGQUIT, SIG_DFL);
    signal (SIGTSTP, SIG_DFL);
    signal (SIGTTIN, SIG_DFL);
    signal (SIGTTOU, SIG_DFL);
    signal (SIGCHLD, SIG_DFL);


  // figure out where to get input/output 
  // especially when piping
  if (in != STDIN_FILENO){
      dup2 (in, STDIN_FILENO);
      close (in);
    }
  if (out != STDOUT_FILENO){
      dup2 (out, STDOUT_FILENO);
      close (out);
    }
  if (err != STDERR_FILENO){
      dup2 (err, STDERR_FILENO);
      close (err);
    }

  // execute the proccess
  execvp (p->argv[0], p->argv);
  perror ("execvp");
  _exit (1); //exit for failed
}

/*set up the job to be run and process execcuted
  inspired by gnu.org*/
void launch_job (JOB *j, int foreground){
  process *p;
  pid_t pid;
  int mypipe[2];
  int in;
  int out;
  in = j->stdin;
  // run for each piped process if it is piped
  for (p = j->first_process; p; p = p->next){
      if (p->next){
          if (pipe (mypipe) < 0){
              exit (1);
            }
          out = mypipe[1];
        }
      else{
        out = j->stdout;
      }

      pid = fork ();
      if (pid == 0){
        // child: execute the process
        launch_process (p, j->pgid, in, out, j->stderr, foreground);
      }
      else if (pid < 0){
          _exit (1);
        }
      else{
          // parent
          p->pid = pid;
          // shell should always be interactive
              if (!j->pgid){
                j->pgid = pid;
              }
              setpgid (pid, j->pgid);                 
        }

      // reset the pipes
      if (in != j->stdin){
        close (in);
      }
      if (out != j->stdout){
        close (out);
      }
      in = mypipe[0];
    }
   
  if (foreground){
    put_job_in_foreground (j, 0);
  }
  else{
    put_job_in_background (j, 1);
  }
}

/*Helper function to set up piped processes*/
void set_process_piping(JOB *j, char *arr[], int arrLength){
  int k = 0;
  process *p = j->first_process;
  for(int i = 0; i < arrLength; i++){
    if(arr[i] == NULL){
      break;
    }
    if(strcmp(arr[i], "|") == 0){
      p->next = (process*)malloc(sizeof(process));
      p = p->next;
      i++;
      k = 0;
    }
    p->argv[k] = malloc(sizeof(char) * 100);
    strcpy(p->argv[k], arr[i]);
    k++;
  }   
}


/*with the new command that is given we now want to create a job to execute*/
void create_job(char *arr[], int arrLength){
  
  JOB *j;
  process *p;
  if((j = malloc(sizeof(JOB))) == NULL) exit(0);
  if((p = malloc(sizeof(process))) == NULL) exit(0);

  // setting up job fields
  j->first_process = p;
  j->stdin = 0;
  j->stdout = 1;
  j->stderr = 2;
  j->first_process = (process*)malloc(sizeof(process) * 64);
  j->background = false;
  j->jobComplete = false;
  j->fromBackground = false;
  j->next = NULL;

  // check if the job will need to be piped, if so we need to set up linked processes
  int check = 0;
  for(int i = 0; i < arrLength; i++){
    if(arr[i] == NULL) break;
    else if(strcmp(arr[i], "|") == 0){
      check = 1;
      set_process_piping(j, arr, arrLength);
    }
  }

  if(!check){
    // want to set the job name/args so we need a deep copy to the argv field
    for(int i = 0; i < arrLength; i++){
      if(arr[i] == NULL){
        break;
      }
      j->first_process->argv[i] = malloc(sizeof(char) * 100);
      strcpy(j->first_process->argv[i], arr[i]);
    }
  }
   if(firstJob == NULL){
    // add the new job into the first job  
    firstJob = j;
  }else{
    // here we need to find the next available slot in the linked job list
    JOB *curr = firstJob;
    while(curr->next != NULL){
      curr = curr->next;
    }
    curr->next = j;
  }

  // check if the job will need to be executed in the background
   int foreground = 1;
   for(int i = 0; i < 254; i++){
    if(j->first_process->argv[i] == NULL){
      break;
    }
    if(strcmp(j->first_process->argv[i], "&") == 0){
      foreground = 0;
      j->first_process->argv[i] = NULL;
      if(j->id < 1){
        j->id = idCounter;
        idCounter++;
      }
      j->background = true;
    }
  }
  launch_job(j, foreground);
}

/*Used for built in command jobs to print the jobs that are in the background*/
void print_jobs(){
  JOB *j = firstJob;
  while(j != NULL){
    // if the job is not complete or stopped then we want to print it... 
    if((j->background && !j->jobComplete) || j->first_process->stopped){
      // need to get all the args for the built in args job
      printf("%d: ", j->id);
      for(int i = 0; i < 10; i++){
        if(j->first_process->argv[i+1] == NULL){
          printf("%s", j->first_process->argv[i]);
          break;
        }
        printf("%s ", j->first_process->argv[i]);
      }
      printf("\n");
    }
      j = j->next;
  }
}

/*when exit is called, need to free the memory alocated to jobs linked list*/
void free_job_memory(){
  
    JOB *job = firstJob;
    int count = 0;
     while (job != NULL) {

        for (int i = 0; i < 254 && job->name[i] != NULL; i++) {
            free(job->name[i]);
        }
        process *currentProcess = job->first_process;
        while (currentProcess != NULL) {
            for (int i = 0; i < 254 && currentProcess->argv[i] != NULL; i++) {
                free(currentProcess->argv[i]);
            }

            process *nextProcess = currentProcess->next;

            free(currentProcess);
            currentProcess = nextProcess;
        }
        
        JOB *nextJob = job->next;
        free(job);
        job = nextJob;
        count++;
    }

}

/*This function handles when the user types command bg*/
void handle_bg(char *line[]){
  // need to parse the input in order to know which job we are looking for
  char *temp = malloc(sizeof(line[1]) * 100);
  strcpy(temp, line[1]);
  char *num = malloc(sizeof(char) * 10);
  int numCount = 0;
  for(int i = 0; i < 5; i++){
    if(temp[i] == '%'){
      continue;
    }
    num[numCount] = temp[i];
    numCount++;
  }

  int digit = atoi(num);

  JOB *j = firstJob;
  JOB *foundJob = NULL;
   while(j != NULL){
    if(j->id == digit){
      foundJob = j;
      break;
    }
      j = j->next;
  }

  // then if the job is found we want to put it into the background
  if(foundJob == NULL){
    return;
  }
  else{
    put_job_in_background(foundJob, 1);
  }
  free(temp);
  free(num); 
}



/*This function handles when the user uses the built in fg command to put a process back into the foreground*/
void handle_fg(char *line[]){
  // need to parse the input in order to know which job we are looking for
  char *temp = malloc(sizeof(line[1]) * 100);
  strcpy(temp, line[1]);
  char *num = malloc(sizeof(char) * 10);
  int numCount = 0;
  for(int i = 0; i < 5; i++){
    if(temp[i] == '%'){
      //the command is %(number) in linux so to be safe I account for that being the command
      continue;
    }
    num[numCount] = temp[i];
    numCount++;
  }

  int digit = atoi(num);

  // then we need to iterate through the jobs and find the job with this id
  JOB *j = firstJob;
  JOB *foundJob = NULL;
   while(j != NULL){
    // if the job is not complete or stopped then we want to print it... 
    if(j->id == digit){
      foundJob = j;
      break;
    }
      j = j->next;
  }

  // if the job is found then we want to put it into the foreground
  if(foundJob == NULL){
    return;
  }
  else{
    foundJob->fromBackground = true;
    put_job_in_foreground(foundJob, 1);
  }
  free(temp);
  free(num); 
}


int main(int argc, char *argv[]){
    // want our shell to ignore these signals. from OS
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    //store shells pid and set up the shell
    shell_pgid = getpid ();
    
    tcsetpgrp (shell_terminal, shell_pgid);

    tcgetattr (shell_terminal, &shell_tmodes);

    //check if we are running in batch mode or not
    bool batchMode = true;
    if(argc > 1){
      batchMode = false;
    }
    
    // if we are in batch mode, we need to be working with a file instead of user input
    if(!batchMode){
      FILE *f = fopen(argv[1], "r");
      if(f == NULL){
        perror("Error opening the file\n");
        exit(0);
      }
      // since printing to stdout we need to be interactivewo work properly
      shell_is_interactive = 1;
      char line[254];
      while(fgets(line, sizeof(line), f) != NULL || !feof(f)){
        if(strlen(line) > 0 && line[strlen(line) - 1] == '\n'){
              line[strlen(line) - 1] = '\0';
        }

        // make a copy of the line that we can adjust as needed then create an array to hold instructions
        char workingLine[sizeof(line)];
        strcpy(workingLine, line);
        char *arr[254];
        int arrLength = 0;

        // using strtok to populate the array
        char *token = strtok(workingLine, " ");
          
          if(token == NULL){
              memset(arr, 0, sizeof(arr));
              continue;
          }
            
          while(token != NULL){
              arr[arrLength] = token;
              token = strtok(NULL, " ");
              arrLength++;
          }

          if(strcmp(arr[0], "exit") == 0){
              free_job_memory();
              _exit(0);
          }
          if(strcmp(arr[0], "cd") == 0){
              handle_cd(arr);
              memset(arr, 0, sizeof(arr));
              continue;
          }
          if(strcmp(arr[0], "jobs") == 0){
            print_jobs();
            continue;
          }
          if(strcmp(arr[0], "fg") == 0){
            handle_fg(arr);
            continue;
          }
          if(strcmp(arr[0], "bg") == 0){
            handle_bg(arr);
            continue;
          }


          create_job(arr, arrLength);
        // reset the array
        memset(arr, 0, sizeof(arr));
        arrLength = 0;

      }
      fclose(f);
      exit(0);
    }

    // using stdin from user to run the shell
    if(batchMode){
      shell_is_interactive = 1;

      char *line = NULL;
      int count = 0;
      while(1){
          size_t len = 0;
          printf("wsh> ");
          count++;
          if(getline(&line, &len, stdin) == -1){
              printf("Error trying to read line, exitting now\n");
              exit(0);
          }
          if(strlen(line) > 0 && line[strlen(line) - 1] == '\n'){
              line[strlen(line) - 1] = '\0';
          }
          fflush(stdin);
          fflush(stdout);

          char *arr[254];
          int counter = 0;
          int arrLength = 0;

          char lineCopy[sizeof(line)];
          strcpy(lineCopy, line);
          char *token = strtok(lineCopy, " ");
          
          if(token == NULL){
              memset(arr, 0, sizeof(arr));
              counter = 0;
              continue;
          }
            
          while(token != NULL){
              arr[arrLength] = token;
              counter++;
              token = strtok(NULL, " ");
              arrLength++;
          }

          for(int i = 0; i < arrLength; i++){
            if(arr[i] == NULL || strcmp(arr[i], " ") == 0) break;
          }

          // check if need to execute specific command
          if(strcmp(arr[0], "exit") == 0){
              free_job_memory();
              break;
          }
          if(strcmp(arr[0], "cd") == 0){
              handle_cd(arr);
              memset(arr, 0, sizeof(arr));
              counter = 0;
              continue;
          }
          if(strcmp(arr[0], "jobs") == 0){
            print_jobs();
            continue;
          }
          if(strcmp(arr[0], "fg") == 0){
            handle_fg(arr);
            continue;
          }
          if(strcmp(arr[0], "bg") == 0){
            handle_bg(arr);
            continue;
          }
         
          // run general instruction
          create_job(arr, arrLength);
        
          //reset the arr to clear
          memset(arr, 0, sizeof(arr));
          counter = 0;
          
      }
    }
        
    exit(0);
}